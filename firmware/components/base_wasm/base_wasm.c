// SPDX-License-Identifier: Apache-2.0
// WASM 宿主在设备上的主程序：作为一个业务跑在 base_runtime 上，一个应用任务独占应用实例和部署状态。
//
// 应用任务阻塞在事件队列上。应用的定时器、业务连接、下载任务，以及系统转来的期望应用和 connect 通知，
// 都只向队列投递事件；部署的决定由纯逻辑的状态机（core/wasm_deploy.c）做出，这里负责执行它给出的动作。
#include "base_wasm.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base_runtime.h"
#include "base_wasm_internal.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "wasm_host.h"
#include "wasm_log.h"
#include "wasm_port.h"

#define TASK_STACK_BYTES 5120 /* 实测最多用到约 2 KB（加载应用时） */
#define TASK_PRIORITY 3
/* 应用任务的看门狗：任务最迟每 10 秒醒一次报到；60 秒没有报到（卡在某次处理里）时运行时重启设备，输出回到安全状态。
 * 60 秒大于一次处理的最长耗时：应用的一次调用受指令预算和睡眠上限约束（几秒），关闭业务连接时代发遗嘱和
 * 等连接任务退出，各自最多等一个网络超时（15 秒）。 */
#define FEED_MS 10000
#define WATCHDOG_MS 60000
#define QUEUE_LENGTH 16
#define QUEUE_RESERVED 6 /* 应用的消息最多占用 QUEUE_LENGTH - QUEUE_RESERVED 个位置，其余留给不能丢的事件 */
#define RESTART_MIN_MS 5000
#define RESTART_MAX_MS 300000
#define LINEAR_MAX_BYTES (16 * 1024) /* 线性内存（栈加静态数据）的上限；超过说明模块没有收缩 */
#define BUDGET 2000000               /* 每次调用的指令预算 */
#define DESIRED_MAX_BYTES 1536
#define LOG_FLUSH_MS 1000 /* 应用写了日志之后最多攒这么久再发：一秒最多一条 app/log 消息 */

_Static_assert(WASM_LOG_BYTES <= BASE_RUNTIME_APP_LOG_BYTES, "an app/log message must fit the system's buffer");
#if CONFIG_ESP_BASE_TEST_WASM_DEPLOY
#define TRIAL_MS 3000u /* 测试固件：缩短试运行 */
#else
#define TRIAL_MS WASM_TRIAL_MS
#endif

static const char *TAG = "base_wasm";
static QueueHandle_t s_queue;
static atomic_bool s_wake_posted; /* 队列里已经有一个 EV_WAKE */
static esp_timer_handle_t s_restart_timer, s_trial_timer, s_retry_timer, s_stable_timer;
static uint32_t s_restart_ms = RESTART_MIN_MS;
static bool s_store_ready;
static unsigned s_boot_actions;

/* 部署状态与应用的运行情况：只在应用任务里读写（启动时由建它的任务初始化，那时应用任务还没建）。 */
static wasm_deploy_t s_deploy;
static wasm_package_t s_packages[WASM_SLOTS];
static struct {
    const char *state;
    char fault[64];
    uint32_t restarts;
} s_app = {.state = "stopped"};

/* 最近一次编码好的 app/state 和攒下的应用日志：应用任务写，系统的控制任务读，s_state_lock 保护。 */
static SemaphoreHandle_t s_state_lock;
static char s_state[WASM_DEPLOY_STATE_BYTES];
static int s_state_length;
static struct {
    char *lines;     /* 攒下的行（JSON 数组的元素）；应用第一次写日志时才分配，之后一直留着 */
    size_t length;
    uint32_t seq, dropped; /* 发出过几条消息；上一条之后攒不下丢掉了几行 */
    bool scheduled;  /* 已经约好了发送 */
} s_log;
static esp_timer_handle_t s_log_timer;
static atomic_bool s_restartable; /* 有应用可以重启（在运行，或被停用了）：控制任务据此应答 app_restart */

static void apply(unsigned actions);

void base_wasm_post(const base_wasm_event_t *event)
{
    /* 应用处理不过来时丢的是它自己的消息：部署、连接状态这些事件要有位置。 */
    const bool room = s_queue && (event->kind != EV_LINK_MESSAGE || uxQueueSpacesAvailable(s_queue) > QUEUE_RESERVED);
    if (room && xQueueSend(s_queue, event, 0) == pdTRUE) return;
    if (event->kind == EV_LINK_MESSAGE) wasm_platform_message_done(event);
    else free(event->data);
    wasm_platform_count_drop();
}

void base_wasm_wake(void)
{
    /* 应用任务处理完每个事件都会去取到期的定时器和引脚，所以队列里有一个叫醒就够了（应用任务取走它时清标记）；
     * 队列满时不用叫：应用任务本来就有事件要处理。 */
    if (!s_queue || atomic_exchange(&s_wake_posted, true)) return;
    const base_wasm_event_t event = {.kind = EV_WAKE};
    if (xQueueSend(s_queue, &event, 0) != pdTRUE) atomic_store(&s_wake_posted, false);
}

#if CONFIG_ESP_BASE_TEST_WASM_BENCH || CONFIG_ESP_BASE_TEST_WASM_DEPLOY
/* 普通 FreeRTOS 任务的入口不能返回：跑完后自己删除。 */
static void plain_task(void *argument)
{
    ((void (*)(void *))argument)(NULL);
    vTaskDelete(NULL);
}
#endif

bool base_wasm_task(const char *name, void (*entry)(void *), uint32_t stack_bytes, uint32_t watchdog_ms)
{
    const esp_err_t result = base_runtime_task(name, entry, NULL, stack_bytes, TASK_PRIORITY, watchdog_ms);
#if CONFIG_ESP_BASE_TEST_WASM_BENCH || CONFIG_ESP_BASE_TEST_WASM_DEPLOY
    /* 测试固件不启动系统层，业务运行时没有在运行：退回普通任务。 */
    if (result == ESP_ERR_INVALID_STATE)
        return xTaskCreate(plain_task, name, stack_bytes, (void *)entry, TASK_PRIORITY, NULL) == pdPASS;
#endif
    return result == ESP_OK;
}

static void timer_start(esp_timer_handle_t timer, uint32_t delay_ms)
{
    esp_timer_stop(timer);
    esp_timer_start_once(timer, (uint64_t)delay_ms * 1000u);
}

static void event_timer_fired(void *argument)
{
    base_wasm_post(&(base_wasm_event_t){.kind = (uint8_t)(uintptr_t)argument});
}

/* ---------- app/state ---------- */

static void publish_state(void)
{
    wasm_host_stats_t stats;
    wasm_host_stats(&stats);
    const wasm_app_status_t app = {.state = s_app.state, .fault = s_app.fault[0] ? s_app.fault : NULL,
                                   .restarts = s_app.restarts, .pool_used = stats.pool_used, .pool_peak = stats.pool_peak,
                                   .linear_bytes = stats.linear_bytes};
    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    s_state_length = wasm_deploy_state_encode(&s_deploy, &app, s_state, sizeof s_state);
    xSemaphoreGive(s_state_lock);
    atomic_store(&s_restartable, s_deploy.running < WASM_SLOTS || s_deploy.disabled);
    ESP_LOGI(TAG, "ESP_BASE_APP job=%s state=%s error=%s app=%s restarts=%u", s_deploy.record.job[0] ? s_deploy.record.job : "-",
             wasm_deploy_phase_name((wasm_deploy_phase_t)s_deploy.record.phase), s_deploy.record.error, s_app.state,
             (unsigned)s_app.restarts);
    base_runtime_app_changed();
}

static int business_state(char *out, size_t size)
{
    int length = 0;
    if (!s_state_lock) return 0;
    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    if (s_state_length > 0 && (size_t)s_state_length < size) {
        memcpy(out, s_state, (size_t)s_state_length + 1);
        length = s_state_length;
    }
    xSemaphoreGive(s_state_lock);
    return length;
}

/* ---------- app/log ---------- */

void base_wasm_log(int level, const char *text, size_t length)
{
    if (!s_state_lock) return;
    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    if (!s_log.lines) s_log.lines = malloc(WASM_LOG_LINES_BYTES);
    if (!s_log.lines ||
        !wasm_log_append(s_log.lines, WASM_LOG_LINES_BYTES, &s_log.length, wasm_port_uptime_ms(), level, text, length))
        ++s_log.dropped; /* 攒不下：应用写得比发得快。丢掉的行数随下一条消息报上去 */
    const bool schedule = !s_log.scheduled;
    s_log.scheduled = true;
    xSemaphoreGive(s_state_lock);
    if (schedule && s_log_timer) timer_start(s_log_timer, LOG_FLUSH_MS);
}

static void log_timer_fired(void *argument)
{
    (void)argument;
    base_runtime_app_logged(); /* 系统随后在控制任务里经 business_log 取走 */
}

static int business_log(char *out, size_t size)
{
    int length = 0;
    if (!s_state_lock) return 0;
    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    if (s_log.length || s_log.dropped) {
        length = wasm_log_encode(out, size, s_log.seq + 1, s_log.dropped, s_log.lines ? s_log.lines : "", s_log.length);
        if (length > 0) ++s_log.seq;
        else length = 0;
        s_log.length = 0;
        s_log.dropped = 0;
    }
    s_log.scheduled = false;
    xSemaphoreGive(s_state_lock);
    return length;
}

/* ---------- 应用的启动与停止 ---------- */

static bool read_slot(void *context, uint8_t *out, uint32_t bytes)
{
    const uint8_t slot = (uint8_t)(uintptr_t)context;
    return wasm_store_read(slot, s_packages[slot].wasm_offset, out, bytes);
}

static void stop_app(void)
{
    esp_timer_stop(s_restart_timer);
    esp_timer_stop(s_stable_timer);
    wasm_host_stop(); /* 取消应用的定时器，代发遗嘱并断开连接 */
}

/* 启动 s_deploy.running 那个槽里的应用。 */
static bool start_app(void)
{
    char error[128] = "";
    const uint8_t slot = s_deploy.running;
    const wasm_package_t *package = &s_packages[slot];
    const wasm_host_limits_t limits = {.pool_bytes = package->memory_bytes, .stack_bytes = package->stack_bytes,
                                       .linear_max_bytes = LINEAR_MAX_BYTES, .budget = BUDGET, .pins = package->pins,
                                       .pin_count = package->pin_count};
    /* 应用代码运行之前，它声明的输出先到安全电平。 */
    wasm_platform_pins(package->pins, package->pin_count);
    if (!wasm_host_start(read_slot, (void *)(uintptr_t)slot, package->wasm_bytes, &limits, error, sizeof error)) {
        ESP_LOGE(TAG, "ESP_BASE_APP_START_FAILED %s %s: %s", package->name, package->version, error);
        strlcpy(s_app.fault, error, sizeof s_app.fault);
        s_app.state = "crashed";
        return false;
    }
    wasm_host_stats_t stats;
    wasm_host_stats(&stats);
    ESP_LOGI(TAG, "ESP_BASE_APP_STARTED %s %s slot=%u wasm=%u pool=%u/%u peak=%u linear=%u", package->name, package->version,
             (unsigned)slot, (unsigned)package->wasm_bytes, (unsigned)stats.pool_used, (unsigned)package->memory_bytes,
             (unsigned)stats.pool_peak, (unsigned)stats.linear_bytes);
    s_app.state = "running";
    timer_start(s_stable_timer, WASM_STABLE_MS);
    return true;
}

static void restart_later(void)
{
    ESP_LOGW(TAG, "ESP_BASE_APP_RESTART in %u ms", (unsigned)s_restart_ms);
    timer_start(s_restart_timer, s_restart_ms);
    s_restart_ms = s_restart_ms * 2 > RESTART_MAX_MS ? RESTART_MAX_MS : s_restart_ms * 2;
}

/* 执行状态机的 RUN：停掉当前应用，启动应当运行的那个。 */
static void run_app(void)
{
    stop_app();
    s_restart_ms = RESTART_MIN_MS;
    if (s_deploy.running >= WASM_SLOTS) {
        s_app.state = s_deploy.disabled ? "disabled" : "stopped";
        if (s_deploy.disabled) snprintf(s_app.fault, sizeof s_app.fault, "crash_loop");
        /* 没有应用了：引脚还给系统。被停用的应用还装在设备上，它的输出继续保持在安全电平。 */
        else wasm_platform_pins(NULL, 0);
        return;
    }
    const uint8_t slot = s_deploy.running;
    const bool ok = start_app();
    /* 试运行的应用启动失败：状态机回退到原来的槽（又是一次 RUN）。 */
    apply(wasm_deploy_on_started(&s_deploy, ok, ok ? NULL : "start_failed"));
    /* 已提交的应用启动失败：退避后再试。 */
    if (!ok && s_deploy.running == slot && !wasm_host_running()) restart_later();
}

/* 把一个事件交给应用；应用出了故障就停掉它，试运行期间回退，否则退避后重启。 */
static void dispatch(wasm_event_kind_t kind, uint32_t a, uint32_t b)
{
    if (!wasm_host_running() || wasm_host_event(kind, a, b)) return;
    strlcpy(s_app.fault, wasm_host_fault(), sizeof s_app.fault);
    ESP_LOGE(TAG, "ESP_BASE_APP_FAULT %s", s_app.fault);
    ++s_app.restarts;
    s_app.state = "crashed";
    stop_app();
    const unsigned actions = wasm_deploy_on_fault(&s_deploy, "fault");
    if (actions) {
        apply(actions);
    } else {
        restart_later();
        publish_state();
    }
}

/* ---------- 部署 ---------- */

static void apply(unsigned actions)
{
    if (actions & WASM_ACT_PERSIST) wasm_store_save_record(&s_deploy.record);
    if (actions & WASM_ACT_CANCEL) wasm_fetch_cancel();
    if (actions & WASM_ACT_STOP_TIMER) esp_timer_stop(s_retry_timer);
    if (actions & WASM_ACT_STOP_TRIAL) esp_timer_stop(s_trial_timer);
    if (actions & WASM_ACT_RUN) run_app();
    if (actions & WASM_ACT_TRIAL_TIMER) timer_start(s_trial_timer, TRIAL_MS);
    if (actions & WASM_ACT_RETRY_TIMER) timer_start(s_retry_timer, wasm_deploy_retry_delay_ms(&s_deploy, esp_random()));
    if (actions & WASM_ACT_DOWNLOAD) {
        /* 固件正在下载时不开始应用的下载：内存留给固件，验签也不会撞在一起。 */
        if (base_runtime_system_updating() || !wasm_fetch_start(&s_deploy.desired, wasm_deploy_target(&s_deploy)))
            apply(wasm_deploy_on_worker_done(&s_deploy, WASM_WORKER_RETRY, "busy", NULL));
    }
    if (actions & WASM_ACT_PUBLISH) publish_state();
}

static void on_desired(const char *payload, size_t length)
{
    if (!s_store_ready) return;
    if (length == 0) {
        apply(wasm_deploy_on_cancel(&s_deploy));
        return;
    }
    /* 解码用的缓冲 600 多字节：不占应用任务的栈，也不常驻。 */
    wasm_desired_t *desired = malloc(sizeof *desired);
    if (!desired) {
        ESP_LOGE(TAG, "no memory to read app/desired"); /* retained：下次连上会再收到 */
        return;
    }
    unsigned actions = 0;
    const char *error = wasm_desired_decode(payload, length, WASM_SLOT_BYTES, desired);
    if (!error) actions = wasm_deploy_on_desired(&s_deploy, desired);
    else if (desired->job[0]) actions = wasm_deploy_on_invalid_desired(&s_deploy, desired->job, error);
    else ESP_LOGW(TAG, "unreadable app/desired ignored");
    free(desired);
    apply(actions);
}

static void on_fetch_done(void)
{
    const wasm_fetch_result_t *result = wasm_fetch_result();
    /* 目标槽被擦过、写过：先让它的旧清单作废，装好了再换成新的。 */
    const uint8_t target = wasm_deploy_target(&s_deploy);
    memset(&s_packages[target], 0, sizeof s_packages[target]);
    if (result->result == WASM_WORKER_INSTALLED) s_packages[target] = result->package;
    apply(wasm_deploy_on_worker_done(&s_deploy, result->result, result->error[0] ? result->error : NULL,
                                     result->result == WASM_WORKER_INSTALLED ? &result->installed : NULL));
}

/* ---------- 应用任务 ---------- */

static void handle(const base_wasm_event_t *event)
{
    switch ((base_wasm_event_kind_t)event->kind) {
    case EV_WAKE: atomic_store(&s_wake_posted, false); break; /* 只用来叫醒任务：到期的定时器和引脚在 drain 里取 */
    case EV_SYSTEM_CONNECT:
        /* 业务 MQTT 的配置变了要重连：先告诉应用连接断了（它可以先把执行机构停下），再去做可能要等网络超时的
         * 关闭和重连。 */
        if (wasm_platform_link_connected()) dispatch(WASM_EVENT_MQTT_DOWN, 0, 0);
        wasm_platform_connect_allowed();
        break;
    case EV_LINK_UP:
        if (wasm_platform_link_up(event->client)) dispatch(WASM_EVENT_MQTT_UP, 0, 0);
        break;
    case EV_LINK_DOWN:
        if (wasm_platform_link_down(event->client)) dispatch(WASM_EVENT_MQTT_DOWN, 0, 0);
        break;
    case EV_LINK_MESSAGE:
        if (wasm_platform_link_current(event->client)) {
            wasm_host_event_data(event->data, event->topic_length, event->data + event->topic_length, event->payload_length);
            dispatch(WASM_EVENT_MQTT_MESSAGE, event->topic_length, event->payload_length);
        }
        wasm_platform_message_done(event);
        break;
    case EV_RESTART:
        if (!wasm_host_running() && s_deploy.running < WASM_SLOTS) {
            if (!start_app()) restart_later();
            publish_state();
        }
        break;
    case EV_APP_RESTART: apply(wasm_deploy_on_restart(&s_deploy)); break;
    case EV_DESIRED:
        on_desired(event->data, event->payload_length);
        free(event->data);
        break;
    case EV_FETCH_VERIFYING: apply(wasm_deploy_on_verifying(&s_deploy)); break;
    case EV_FETCH_DONE: on_fetch_done(); break;
    case EV_TRIAL_TIMER: apply(wasm_deploy_on_trial_timer(&s_deploy)); break;
    case EV_RETRY_TIMER: apply(wasm_deploy_on_retry_timer(&s_deploy)); break;
    case EV_STABLE_TIMER:
        /* 应用稳定运行了一段时间：异常重启的计数清零，故障后的重启间隔从头算。 */
        if (wasm_host_running()) {
            s_restart_ms = RESTART_MIN_MS;
            apply(wasm_deploy_on_stable(&s_deploy));
        }
        break;
    }
}

/* 把到期的定时器和时限到了的输出脚交给应用。先报引脚：它关系到执行机构的状态。 */
static void drain(void)
{
    unsigned timers, pins;
    wasm_platform_take(&timers, &pins);
    for (unsigned index = 0; pins; ++index, pins >>= 1)
        if (pins & 1u) dispatch(WASM_EVENT_PIN_EXPIRED, wasm_platform_pin(index), 0);
    for (unsigned id = 0; timers; ++id, timers >>= 1)
        if (timers & 1u) dispatch(WASM_EVENT_TIMER, id, 0);
}

static void app_task(void *argument)
{
    (void)argument;
    apply(s_boot_actions | WASM_ACT_PUBLISH);
    for (base_wasm_event_t event;;) {
        if (xQueueReceive(s_queue, &event, pdMS_TO_TICKS(FEED_MS)) == pdTRUE) handle(&event);
        drain();
        base_runtime_feed();
    }
}

/* 队列、定时器、应用槽和部署记录：在建应用任务之前、由启动它的任务完成。 */
static bool prepare(const char *device_id)
{
    static const struct {
        esp_timer_handle_t *timer;
        base_wasm_event_kind_t kind;
        const char *name;
    } timers[] = {{&s_restart_timer, EV_RESTART, "app_restart"}, {&s_trial_timer, EV_TRIAL_TIMER, "app_trial"},
                  {&s_retry_timer, EV_RETRY_TIMER, "app_retry"}, {&s_stable_timer, EV_STABLE_TIMER, "app_stable"}};
    s_queue = xQueueCreate(QUEUE_LENGTH, sizeof(base_wasm_event_t));
    s_state_lock = xSemaphoreCreateMutex();
    bool ready = s_queue && s_state_lock && wasm_platform_init(device_id);
    for (size_t index = 0; ready && index < sizeof timers / sizeof timers[0]; ++index) {
        const esp_timer_create_args_t timer = {.callback = event_timer_fired, .arg = (void *)(uintptr_t)timers[index].kind,
                                               .name = timers[index].name};
        ready = esp_timer_create(&timer, timers[index].timer) == ESP_OK;
    }
    const esp_timer_create_args_t log_timer = {.callback = log_timer_fired, .name = "app_log"};
    if (!ready || esp_timer_create(&log_timer, &s_log_timer) != ESP_OK) return false;

    /* 读两个槽并验签。这一步在系统的控制任务里做，排在系统自己校验运行固件之后：官方的验签入口不可重入。 */
    wasm_slot_t slots[WASM_SLOTS] = {0};
    s_store_ready = wasm_store_init();
    for (uint8_t slot = 0; s_store_ready && slot < WASM_SLOTS; ++slot) {
        const char *error = NULL;
        if (wasm_store_inspect(slot, &slots[slot], &s_packages[slot], &error))
            ESP_LOGI(TAG, "ESP_BASE_APP_SLOT %u %s %s sha256=%s", (unsigned)slot, slots[slot].name, slots[slot].version,
                     slots[slot].sha256);
        else
            ESP_LOGI(TAG, "ESP_BASE_APP_SLOT %u empty (%s)", (unsigned)slot, error);
    }
    static wasm_deploy_record_t stored;
    const bool have_record = s_store_ready && wasm_store_load_record(&stored);
#if CONFIG_ESP_BASE_TEST_WASM_BENCH || CONFIG_ESP_BASE_TEST_WASM_DEPLOY
    /* 测试固件不启动系统层：自己看复位原因。 */
    const esp_reset_reason_t reset = esp_reset_reason();
    const bool abnormal = reset == ESP_RST_PANIC || reset == ESP_RST_INT_WDT || reset == ESP_RST_TASK_WDT || reset == ESP_RST_WDT;
#else
    /* 与系统的业务守卫用同一个判定：panic、各类看门狗，以及应用任务的看门狗超时。 */
    const bool abnormal = base_runtime_abnormal_restart();
#endif
    s_boot_actions = wasm_deploy_boot(&s_deploy, have_record ? &stored : NULL, slots, abnormal);
    /* 联网之前就把要运行的应用声明的输出置到安全电平；应用被停用时同样如此。 */
    const uint8_t owner = s_deploy.running < WASM_SLOTS ? s_deploy.running : s_deploy.record.active;
    if (owner < WASM_SLOTS) wasm_platform_pins(s_packages[owner].pins, s_packages[owner].pin_count);
    if (s_deploy.disabled)
        ESP_LOGE(TAG, "ESP_BASE_APP_DISABLED the device restarted abnormally %u times in a row with this app",
                 (unsigned)s_deploy.record.crashes);
    return true;
}

static void business_init(void)
{
    if (!prepare(base_runtime_device_id()) || !base_wasm_task("wasm_app", app_task, TASK_STACK_BYTES, WATCHDOG_MS))
        ESP_LOGE(TAG, "ESP_BASE_APP_UNAVAILABLE resources for the WASM host could not be created");
}

static void business_connect(void)
{
    if (s_queue) base_wasm_post(&(base_wasm_event_t){.kind = EV_SYSTEM_CONNECT});
}

/* 系统转来的 app/desired：在控制任务里调用，只拷贝并投递。 */
static void business_deploy(const char *payload, size_t length)
{
    if (!s_queue || length > DESIRED_MAX_BYTES) return;
    base_wasm_event_t event = {.kind = EV_DESIRED, .payload_length = (uint16_t)length};
    if (length) {
        if (!(event.data = malloc(length))) return;
        memcpy(event.data, payload, length);
    }
    base_wasm_post(&event);
}

/* 命令 app_restart：在控制任务里调用。没有应用可以重启时拒绝，否则交给应用任务。 */
static bool business_restart(void)
{
    if (!s_queue || !atomic_load(&s_restartable)) return false;
    base_wasm_post(&(base_wasm_event_t){.kind = EV_APP_RESTART});
    return true;
}

static const base_app_channel_t s_channel = {.deploy = business_deploy, .state = business_state, .log = business_log,
                                             .restart = business_restart};
const base_business_t base_wasm_business = {.name = "wasm", .init = business_init, .connect = business_connect,
                                            .app = &s_channel};

/* ---------- 测试固件 ---------- */
#if CONFIG_ESP_BASE_TEST_WASM_BENCH || CONFIG_ESP_BASE_TEST_WASM_DEPLOY
#include "esp_heap_caps.h"

static void report(const char *tag, const char *phase)
{
    wasm_host_stats_t stats;
    wasm_host_stats(&stats);
    printf("%s phase=%s heap_free=%u heap_min=%u heap_largest=%u pool_used=%u pool_peak=%u linear=%u stack_min_free=%u\n",
           tag, phase, (unsigned)heap_caps_get_free_size(MALLOC_CAP_DEFAULT),
           (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_DEFAULT),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT), (unsigned)stats.pool_used,
           (unsigned)stats.pool_peak, (unsigned)stats.linear_bytes, (unsigned)uxTaskGetStackHighWaterMark(NULL));
}
#endif

#if CONFIG_ESP_BASE_TEST_WASM_BENCH
/* 内存实测（darren-space#113 T3）：不启动系统层，反复启动、运行、停止内嵌的样例应用。 */
#define BENCH_CALLS 1000
#define BENCH_ROUNDS 3
#define BENCH_POOL_BYTES (10 * 1024)
#define BENCH_STACK_BYTES 2048

extern const uint8_t hello_start[] asm("_binary_hello_wasm_start");
extern const uint8_t hello_end[] asm("_binary_hello_wasm_end");

static bool read_embedded(void *context, uint8_t *out, uint32_t bytes)
{
    (void)context;
    memcpy(out, hello_start, bytes);
    return true;
}

static void bench_task(void *argument)
{
    (void)argument;
    static const char topic[] = "hello/in", payload[] = "{\"ping\":1}";
    const uint32_t bytes = (uint32_t)(hello_end - hello_start);
    const wasm_host_limits_t limits = {.pool_bytes = BENCH_POOL_BYTES, .stack_bytes = BENCH_STACK_BYTES,
                                       .linear_max_bytes = LINEAR_MAX_BYTES, .budget = BUDGET};
#if CONFIG_WAMR_INTERP_FAST
    const char *interp = "fast";
#else
    const char *interp = "classic";
#endif
    printf("WASM_BENCH begin target=%s interp=%s image_bytes=%u pool_bytes=%u wasm_stack=%u task_stack=%u\n",
           CONFIG_IDF_TARGET, interp, (unsigned)bytes, (unsigned)BENCH_POOL_BYTES, (unsigned)BENCH_STACK_BYTES,
           (unsigned)TASK_STACK_BYTES);
    s_queue = xQueueCreate(QUEUE_LENGTH, sizeof(base_wasm_event_t));
    bool ok = s_queue && wasm_platform_init("bench");
    report("WASM_BENCH", "idle");
    for (int round = 0; ok && round < BENCH_ROUNDS; ++round) {
        char error[128] = "";
        ok = wasm_host_start(read_embedded, NULL, bytes, &limits, error, sizeof error);
        if (!ok) printf("WASM_BENCH failed=%s\n", error);
        report("WASM_BENCH", "started");
        const int64_t begin = esp_timer_get_time();
        ok = ok && wasm_host_event(WASM_EVENT_MQTT_UP, 0, 0);
        for (int index = 0; ok && index < BENCH_CALLS; ++index) ok = wasm_host_event(WASM_EVENT_TIMER, 0, 0);
        const int64_t elapsed = esp_timer_get_time() - begin;
        wasm_host_event_data(topic, sizeof topic - 1, payload, sizeof payload - 1);
        ok = ok && wasm_host_event(WASM_EVENT_MQTT_MESSAGE, sizeof topic - 1, sizeof payload - 1);
        wasm_host_stats_t stats;
        wasm_host_stats(&stats);
        printf("WASM_BENCH round=%d ok=%d calls=%u faults=%u per_call_us=%d\n", round, ok, (unsigned)stats.calls,
               (unsigned)stats.faults, (int)(elapsed / BENCH_CALLS));
        report("WASM_BENCH", "events");
        wasm_host_stop();
        report("WASM_BENCH", "stopped"); /* 与 idle 相同才说明没有泄漏 */
    }
    printf("WASM_BENCH done ok=%d dropped=%u\n", ok, wasm_platform_dropped());
    vTaskDelete(NULL);
}

void base_wasm_bench(void)
{
    xTaskCreate(bench_task, "wasm_app", TASK_STACK_BYTES, NULL, TASK_PRIORITY, NULL);
    for (;;) vTaskDelay(portMAX_DELAY);
}
#endif

#if CONFIG_ESP_BASE_TEST_WASM_DEPLOY
/* 部署的设备级测试：不启动系统层，没有网络。待部署的应用包由测试脚本预先放进 Flash（ota_1 的位置，每个占
 * 一个槽的大小），期望应用按脚本给的顺序逐个下发；应用槽、验签、试运行、提交、回退和部署记录走的都是正式的
 * 代码，只有「下载」换成了从 Flash 里拷贝（wasm_store.c）。步骤记在 NVS 里，中途的重启不打断脚本。 */
#include "esp_flash.h"
#include "esp_rom_sys.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "psa/crypto.h"

#define STAGED_OFFSET 0x200000u
#define EXTRA_PACKAGE 8

/* 预置区的第 index 个位置上有没有应用包。 */
static bool staged(unsigned index)
{
    uint8_t magic[4];
    return esp_flash_read(NULL, magic, STAGED_OFFSET + index * WASM_SLOT_BYTES, sizeof magic) == ESP_OK &&
           !memcmp(magic, WASM_PACKAGE_MAGIC, sizeof magic);
}

static uint32_t script_step(void)
{
    nvs_handle_t handle;
    uint32_t step = 0;
    if (nvs_open("app_test", NVS_READONLY, &handle) == ESP_OK) {
        nvs_get_u32(handle, "step", &step);
        nvs_close(handle);
    }
    return step;
}

static void script_save(uint32_t step)
{
    nvs_handle_t handle;
    if (nvs_open("app_test", NVS_READWRITE, &handle) != ESP_OK) return;
    nvs_set_u32(handle, "step", step);
    nvs_commit(handle);
    nvs_close(handle);
}

/* 把预置的第 index 个包作为期望应用下发：任务号按步骤生成，摘要和大小取自包本身。 */
static bool stage(unsigned step, unsigned index)
{
    static uint8_t head[WASM_PACKAGE_HEADER_BYTES + WASM_PACKAGE_MANIFEST_MAX];
    static char payload[512];
    wasm_package_t package;
    uint8_t digest[32];
    size_t digest_length = 0;
    char sha256[65] = "";
    const uint32_t source = STAGED_OFFSET + index * WASM_SLOT_BYTES;
    if (esp_flash_read(NULL, head, source, sizeof head) != ESP_OK) return false;
    /* 包头读不出来的（故意放的坏包）按最小的整包下发，让正式的校验去拒绝它。 */
    const uint32_t size = wasm_package_parse(head, sizeof head, WASM_SLOT_BYTES, &package) ? 2 * WASM_PACKAGE_SECTOR_BYTES
                                                                                         : package.total_bytes;
    uint8_t *content = malloc(size);
    const bool hashed = content && esp_flash_read(NULL, content, source, size) == ESP_OK && psa_crypto_init() == PSA_SUCCESS &&
                        psa_hash_compute(PSA_ALG_SHA_256, content, size, digest, sizeof digest, &digest_length) == PSA_SUCCESS;
    free(content);
    if (!hashed) return false;
    for (int at = 0; at < 32; ++at) snprintf(sha256 + at * 2, 3, "%02x", digest[at]);
    const int length = snprintf(payload, sizeof payload,
                                "{\"job\":\"00000000-0000-4000-8000-%012u\",\"name\":\"staged\",\"version\":\"0.0.0+%u\","
                                "\"abi\":1,\"size\":%u,\"sha256\":\"%s\",\"url\":\"https://staged.test/%u\"}",
                                step, index, (unsigned)size, sha256, index);
    printf("WASM_DEPLOY_TEST step=%u deploy package=%u size=%u\n", step, index, (unsigned)size);
    business_deploy(payload, (size_t)length);
    return true;
}

static void script_task(void *argument)
{
    (void)argument;
    /* 每一步：下发哪个包（-1 表示只重启设备），等多久看结果。包的内容和顺序见 tests/wasm_deploy_qemu.py。 */
    static const struct {
        int package;
        uint32_t wait_ms;
    } steps[] = {{0, 6000}, {-1, 0}, {1, 6000}, {2, 6000}, {3, 3000}, {4, 3000}, {5, 8000}, {6, 6000}, {-1, 0}, {7, 6000}, {-1, 0},
                 {EXTRA_PACKAGE, 8000}}; /* 最后一步可选：脚本另外放了一个包才做（实板上量真实应用的内存） */
    vTaskDelay(pdMS_TO_TICKS(1500)); /* 等应用任务按部署记录把该运行的应用拉起来 */
    for (unsigned step = script_step(); step < sizeof steps / sizeof steps[0]; ++step) {
        script_save(step + 1);
        printf("WASM_DEPLOY_TEST step=%u before=%s\n", step, s_state);
        if (steps[step].package < 0) {
            printf("WASM_DEPLOY_TEST step=%u restart\n", step);
            vTaskDelay(pdMS_TO_TICKS(200));
#if CONFIG_IDF_TARGET_ESP32
            /* QEMU 的 ESP32 在只复位 CPU 的重启（esp_restart）之后不复位定时器组，遗留的中断让 ESP-IDF 在启动时崩溃；
             * 这里改用整个系统的软件复位。实板不受影响，正式代码仍用 esp_restart。 */
            esp_rom_software_reset_system();
#else
            esp_restart();
#endif
        }
        if (steps[step].package == EXTRA_PACKAGE && !staged(EXTRA_PACKAGE)) {
            printf("WASM_DEPLOY_TEST step=%u skipped\n", step);
            continue;
        }
        if (!stage(step, (unsigned)steps[step].package)) printf("WASM_DEPLOY_TEST step=%u stage_failed\n", step);
        vTaskDelay(pdMS_TO_TICKS(steps[step].wait_ms));
        report("WASM_DEPLOY_TEST", "step");
        printf("WASM_DEPLOY_TEST step=%u after=%s\n", step, s_state);
    }
    report("WASM_DEPLOY_TEST", "final");
    printf("WASM_DEPLOY_TEST done final=%s\n", s_state);
    vTaskDelete(NULL);
}

void base_wasm_deploy_test(void)
{
    esp_err_t result = nvs_flash_init();
    if (result == ESP_ERR_NVS_NO_FREE_PAGES || result == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        result = nvs_flash_init();
    }
    printf("WASM_DEPLOY_TEST begin target=%s step=%u nvs=%s\n", CONFIG_IDF_TARGET, (unsigned)script_step(),
           esp_err_to_name(result));
    /* prepare 就是正式固件里业务的 init 回调做的事（登记应用槽、逐槽验签、读部署记录）：量一下它要多久。 */
    const int64_t begin_us = esp_timer_get_time();
    const bool prepared = prepare("deploy-test");
    printf("WASM_DEPLOY_TEST prepare_ms=%d\n", (int)((esp_timer_get_time() - begin_us) / 1000));
    if (!prepared || !base_wasm_task("wasm_app", app_task, TASK_STACK_BYTES, 0)) printf("WASM_DEPLOY_TEST failed=prepare\n");
    report("WASM_DEPLOY_TEST", "prepared");
    xTaskCreate(script_task, "script", 6144, NULL, TASK_PRIORITY, NULL);
    for (;;) vTaskDelay(portMAX_DELAY);
}
#endif
