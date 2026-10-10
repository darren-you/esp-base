"""直接编译锁定 SDK 的 HTTP parser、追加、回调、fetch 和清理链；不访问网络或设备。"""
from pathlib import Path
import argparse
import hashlib
import json
import os
import re
import subprocess
import tempfile

from check_sdk import check


def function(source, anchor):
    # Select the definition, never an earlier forward declaration. Locked SDK
    # functions close at column zero; nested blocks and literals cannot close it.
    match = re.search(r'(?m)^' + re.escape(anchor) + r'[^;{]*\{', source)
    if match is None:
        raise ValueError('SDK function definition missing: ' + anchor)
    return source[match.start():source.index('\n}', match.end()) + 2]


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--idf-path', type=Path, required=True)
args = parser.parse_args()
check(args.idf_path)
root = Path(__file__).resolve().parent.parent
cmake = (root / 'firmware/CMakeLists.txt').read_text()
budget = int(re.search(r'target_compile_definitions\(\$\{esp_base_http_parser_library\} PRIVATE HTTP_MAX_HEADER_SIZE=(\d+)\)', cmake)[1])
assert budget == 8192
client_path = args.idf_path / 'components/esp_http_client/esp_http_client.c'
utils_path = args.idf_path / 'components/esp_http_client/lib/http_utils.c'
parser_path = args.idf_path / 'components/http_parser/http_parser.c'
client_source = client_path.read_text()
utils_source = utils_path.read_text()
parts = [function(utils_source, 'char *http_utils_append_string(')]
parts += [function(client_source, anchor) for anchor in (
    'static int http_on_message_begin(', 'static int http_on_status(',
    'static int http_on_header_event(', 'static int http_on_header_field(',
    'static int http_on_header_value(', 'static int http_on_headers_complete(',
    'static int http_on_body(', 'static void esp_http_client_cached_buf_cleanup(',
    'esp_err_t esp_http_client_cleanup(', 'int64_t esp_http_client_fetch_headers(')]

prefix = r'''
#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "http_parser.h"
#define CONFIG_ESP_HTTP_CLIENT_SAVE_RESPONSE_HEADERS 0
#define CONFIG_ESP_HTTP_CLIENT_ENABLE_GET_CONTENT_RANGE 0
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_HTTP_EAGAIN 11
#define ERR_TCP_TRANSPORT_CONNECTION_TIMEOUT -3
#define ESP_LOGD(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define ESP_RETURN_ON_FALSE(a,r,...) do { if (!(a)) return (r); } while (0)
#define HTTP_RET_ON_FALSE_DBG ESP_RETURN_ON_FALSE
enum { HTTP_EVENT_ON_STATUS_CODE, HTTP_EVENT_ON_HEADER, HTTP_EVENT_ON_HEADERS_COMPLETE, HTTP_EVENT_ON_DATA };
enum { HTTP_STATE_REQ_COMPLETE_HEADER=1, HTTP_STATE_REQ_COMPLETE_DATA, HTTP_STATE_RES_COMPLETE_HEADER, HTTP_STATE_RES_ON_DATA_START };
#define HTTP_METHOD_HEAD 2
typedef int esp_err_t;
typedef struct { char *data, *output_ptr, *orig_raw_data, *raw_data; int len; size_t raw_len; } esp_http_buffer_t;
typedef struct { void *headers; esp_http_buffer_t *buffer; int status_code, data_offset; int64_t content_length, data_process; bool is_chunked; } http_message_t;
typedef struct esp_http_client {
    char *current_header_key, *current_header_value, *location, *auth_header;
    struct { char *header_key, *header_value; } event;
    struct { int method; } connection_info;
    http_message_t *request, *response;
    int state, buffer_size_rx, timeout_ms;
    bool is_chunk_complete, cache_data_in_fetch_hdr;
    void *transport_list, *transport, *if_name, *auth_data;
    http_parser *parser;
    http_parser_settings *parser_settings;
} esp_http_client_t;
typedef esp_http_client_t *esp_http_client_handle_t;
typedef struct { int64_t data_process; esp_http_client_handle_t client; } esp_http_client_on_data_t;
static void http_dispatch_event(void *c, int e, void *p, size_t n) { (void)c;(void)e;(void)p;(void)n; }
static void http_dispatch_event_to_event_loop(int e, void *p, size_t n) { (void)e;(void)p;(void)n; }
static void esp_transport_list_destroy(void *p) { assert(!p); }
static void http_header_destroy(void *p) { assert(!p); }
static void _clear_connection_info(void *p) { (void)p; }
static void _clear_auth_data(void *p) { (void)p; }
static unsigned close_calls;
static void esp_http_client_close(void *p) { assert(p); ++close_calls; }
typedef struct { void *ptr; size_t size; } allocation_t;
static allocation_t allocations[32];
static size_t live, peak, maximum_request, allocation_calls, fail_call;
static void *tracked_malloc(size_t n)
{
    ++allocation_calls;
    if (fail_call && allocation_calls==fail_call) return NULL;
    void *p=malloc(n ? n : 1); assert(p);
    for (unsigned i=0;i<32;++i) if (!allocations[i].ptr) {
        allocations[i]=(allocation_t){p,n}; live+=n;
        if (live>peak) peak=live;
        if (n>maximum_request) maximum_request=n;
        return p;
    }
    abort();
}
static void tracked_free(void *p)
{
    if (!p) return;
    for (unsigned i=0;i<32;++i) if (allocations[i].ptr==p) {
        live-=allocations[i].size; allocations[i]=(allocation_t){0}; free(p); return;
    }
    abort();
}
static void *tracked_calloc(size_t n, size_t s)
{ void *p=tracked_malloc(n*s); if (p) memset(p,0,n*s); return p; }
static void *tracked_realloc(void *old, size_t n)
{
    size_t previous=0;
    if (old) {
        bool found=false;
        for (unsigned i=0;i<32;++i) if (allocations[i].ptr==old) { previous=allocations[i].size; found=true; }
        assert(found);
    }
    /* Force the real moving-realloc coexistence instead of relying on host in-place growth. */
    void *p=tracked_malloc(n);
    if (!p) return NULL;
    if (old) memcpy(p,old,previous<n ? previous : n);
    tracked_free(old); return p;
}
static const char *wire;
static size_t wire_size, wire_offset, fragment;
static int esp_transport_read(void *t, char *buffer, int limit, int timeout)
{
    (void)t;(void)timeout;
    size_t n=wire_size-wire_offset;
    if (n>fragment) n=fragment;
    if (n>(size_t)limit) n=(size_t)limit;
    memcpy(buffer,wire+wire_offset,n); wire_offset+=n;
    return (int)n;
}
#define malloc tracked_malloc
#define calloc tracked_calloc
#define realloc tracked_realloc
#define free tracked_free
'''

suffix = r'''
static size_t string_size(const char *s) { return s ? strlen(s)+1 : 0; }
static size_t key_max, value_max, auxiliary_max, headers_live_max;
static void snapshot(http_parser *parser)
{
    esp_http_client_t *c=parser->data;
    size_t k=string_size(c->current_header_key),v=string_size(c->current_header_value);
    size_t a=string_size(c->location)+string_size(c->auth_header);
    if (k>key_max) key_max=k;
    if (v>value_max) value_max=v;
    if (a>auxiliary_max) auxiliary_max=a;
    if (k+v+a>headers_live_max) headers_live_max=k+v+a;
}
static int field(http_parser *p,const char *s,size_t n) { int r=http_on_header_field(p,s,n);snapshot(p);return r; }
static int value(http_parser *p,const char *s,size_t n) { int r=http_on_header_value(p,s,n);snapshot(p);return r; }
static int headers(http_parser *p) { snapshot(p);return http_on_headers_complete(p); }
static esp_http_client_t *new_client(void)
{
    esp_http_client_t *c=calloc(1,sizeof(*c));
    c->request=calloc(1,sizeof(*c->request));c->response=calloc(1,sizeof(*c->response));
    c->request->buffer=calloc(1,sizeof(esp_http_buffer_t));c->response->buffer=calloc(1,sizeof(esp_http_buffer_t));
    c->request->buffer->data=calloc(1,512);c->response->buffer->data=calloc(1,1024);
    c->parser=calloc(1,sizeof(http_parser));c->parser_settings=calloc(1,sizeof(http_parser_settings));
    c->parser_settings->on_message_begin=http_on_message_begin;c->parser_settings->on_status=http_on_status;
    c->parser_settings->on_header_field=field;c->parser_settings->on_header_value=value;
    c->parser_settings->on_headers_complete=headers;c->parser_settings->on_body=http_on_body;
    http_parser_init(c->parser,HTTP_RESPONSE);c->parser->data=c;
    c->state=HTTP_STATE_REQ_COMPLETE_HEADER;c->buffer_size_rx=1024;c->timeout_ms=3000;c->cache_data_in_fetch_hdr=true;
    return c;
}
int main(int argc,char **argv)
{
    assert(argc==5);
    const char *kind=argv[1]; size_t length=strtoul(argv[2],NULL,10);
    fragment=strtoul(argv[3],NULL,10);assert(fragment>=1 && fragment<=1024);
    size_t requested_failure=strtoul(argv[4],NULL,10);
    /* Wire is an external input; it is not part of the client allocation audit. */
    char *input=tracked_malloc(length+16384);size_t external=live;
    size_t cursor=0;
    if (!strcmp(kind,"interim")) cursor+=(size_t)snprintf(input,length+16384,"HTTP/1.1 100 Continue\r\nLocation: first\r\n\r\n");
    cursor+=(size_t)snprintf(input+cursor,length+16384-cursor,"HTTP/1.1 200 OK\r\nContent-Length: 1118208\r\n");
    if (!strcmp(kind,"field")) {
        memset(input+cursor,'X',length);cursor+=length;
        cursor+=(size_t)snprintf(input+cursor,length+16384-cursor,": a\r\n\r\n");
    } else if (!strcmp(kind,"repeated")) {
        for (unsigned i=0;i<4;++i) {
            const char *name=i%2 ? "WWW-Authenticate" : "Location";
            cursor+=(size_t)snprintf(input+cursor,length+16384-cursor,"%s: ",name);
            size_t n=length/4+(i==3 ? length%4 : 0);
            memset(input+cursor,'a',n);cursor+=n;
            cursor+=(size_t)snprintf(input+cursor,length+16384-cursor,"\r\n");
        }
        cursor+=(size_t)snprintf(input+cursor,length+16384-cursor,"\r\n");
    } else if (!strcmp(kind,"fold")) {
        cursor+=(size_t)snprintf(input+cursor,length+16384-cursor,"Location: ");
        size_t first=length/2;
        memset(input+cursor,'a',first);cursor+=first;
        cursor+=(size_t)snprintf(input+cursor,length+16384-cursor,"\r\n ");
        memset(input+cursor,'a',length-first);cursor+=length-first;
        cursor+=(size_t)snprintf(input+cursor,length+16384-cursor,"\r\n\r\n");
    } else {
        const char *name=(!strcmp(kind,"location") || !strcmp(kind,"interim")) ? "Location" : !strcmp(kind,"auth") ? "WWW-Authenticate" : "X";
        cursor+=(size_t)snprintf(input+cursor,length+16384-cursor,"%s: ",name);
        memset(input+cursor,'a',length);cursor+=length;
        cursor+=(size_t)snprintf(input+cursor,length+16384-cursor,"\r\n\r\n");
    }
    if (!strcmp(kind,"body")) { memset(input+cursor,'b',1024);cursor+=1024; }
    esp_http_client_t *c=new_client();size_t base=live-external;
    peak=live;maximum_request=0;allocation_calls=0;fail_call=requested_failure;
    wire=input;wire_size=cursor;
    int64_t result=esp_http_client_fetch_headers(c);
    int error=HTTP_PARSER_ERRNO(c->parser);
    size_t max_request=maximum_request, dynamic_peak=peak-external-base;
    size_t remaining=live-external-base;
    size_t cached=c->response->buffer->raw_len;
    assert(fragment<=1024 && cached<=1024);
    if (HTTP_MAX_HEADER_SIZE==8192) {
        /* A callback may precede the final COUNT for this RX; fetch stops after the first completed header. */
        const size_t bound=HTTP_MAX_HEADER_SIZE+1024;
        assert(max_request<=bound+1);
        assert(headers_live_max<=2*bound+4);
        assert(dynamic_peak<=3*bound+5+1024);
    }
    assert(esp_http_client_cleanup(c)==ESP_OK && close_calls==1);
    assert(live==external);
    printf("{\"kind\":\"%s\",\"value_or_field_bytes\":%zu,\"fragment_bytes\":%zu,\"fail_call\":%zu,\"error\":\"%s\",\"fetch_result\":%"PRId64",\"wire_consumed\":%zu,\"maximum_request_bytes\":%zu,\"key_max_bytes\":%zu,\"value_max_bytes\":%zu,\"auxiliary_max_bytes\":%zu,\"headers_live_max_bytes\":%zu,\"header_and_cache_moving_peak_bytes\":%zu,\"header_and_cache_remaining_bytes\":%zu,\"cached_body_bytes\":%zu,\"cleanup_leaked_bytes\":0}\n",
        kind,length,fragment,requested_failure,http_errno_name(error),result,wire_offset,max_request,key_max,value_max,auxiliary_max,headers_live_max,dynamic_peak,remaining,cached);
    free(input);assert(!live);return 0;
}
'''

with tempfile.TemporaryDirectory(prefix='esp-base-http-header-') as directory:
    work = Path(directory)
    source = work / 'actual_client_test.c'
    source.write_text(prefix + '\n\n'.join(parts) + suffix)
    common = [os.environ.get('CC', 'cc'), '-std=c11', '-Wall', '-Wextra', '-Werror',
              '-Wno-unused-parameter', '-fsanitize=address,undefined',
              '-I' + str(parser_path.parent), str(source), str(parser_path)]
    bounded = work / 'bounded'
    baseline = work / 'sdk_default'
    subprocess.run(common + ['-DHTTP_MAX_HEADER_SIZE=' + str(budget), '-o', str(bounded)], check=True)
    subprocess.run(common + ['-o', str(baseline)], check=True)

    def run(binary, kind, size, fragment=1024, failure=0):
        result = subprocess.run([str(binary), kind, str(size), str(fragment), str(failure)],
                                check=True, capture_output=True, text=True)
        return json.loads(result.stdout)

    rows = []
    old = run(baseline, 'value', 80000)
    assert old['error'] == 'HPE_OK' and old['fetch_result'] == 1118208
    assert old['maximum_request_bytes'] == 80001
    rows.append({'sdk_default_counterexample': old})
    for fragment in (1, 17, 1024):
        low, high = 0, budget + 1024
        while low + 1 < high:
            middle = (low + high) // 2
            if run(bounded, 'value', middle, fragment)['error'] == 'HPE_OK':
                low = middle
            else:
                high = middle
        accepted, rejected = run(bounded, 'value', low, fragment), run(bounded, 'value', high, fragment)
        assert accepted['fetch_result'] == 1118208
        assert rejected['error'] == 'HPE_HEADER_OVERFLOW' and rejected['fetch_result'] == -1
        rows.append({'adjacent_boundary': [accepted, rejected]})
        for kind in ('field', 'location', 'auth', 'repeated', 'fold', 'interim', 'body'):
            accepted = run(bounded, kind, 2000, fragment)
            rejected = run(bounded, kind, 10000, fragment)
            assert accepted['error'] == 'HPE_OK'
            if kind == 'interim':
                # fetch_headers stops after the first completed 1xx header;
                # only the remainder of that very same RX is parsed afterwards.
                assert rejected['error'] == 'HPE_OK'
                assert rejected['wire_consumed'] <= 1024
            else:
                assert rejected['error'] == 'HPE_HEADER_OVERFLOW'
            rows.extend([accepted, rejected])
    # Near-budget auxiliary duplication and a body sharing the last header RX.
    for kind in ('location', 'auth', 'repeated', 'fold', 'body'):
        near = run(bounded, kind, 8000)
        assert near['error'] == 'HPE_OK' and near['fetch_result'] == 1118208
        rows.append(near)
    # Allocation failures in ordinary, duplicated Location/auth and first-body-cache paths.
    for kind in ('value', 'location', 'auth', 'body'):
        for failure in (1, 2, 3, 7):
            failed = run(bounded, kind, 7000, 1024, failure)
            assert failed['error'].startswith('HPE_CB_') and failed['cleanup_leaked_bytes'] == 0
            rows.append(failed)
    print(json.dumps({'budget_bytes': budget, 'rx_bytes': 1024, 'tx_bytes': 512,
                      'current_formal_config': {'save_response_headers': False, 'get_content_range': False},
                      'conservative_request_bound_bytes': budget + 1024 + 1,
                      'header_strings_live_bound_bytes': 2 * (budget + 1024) + 4,
                      'header_strings_moving_peak_bound_bytes': 3 * (budget + 1024) + 5,
                      'strings_rx_tx_and_cached_body_live_bound_bytes': 2 * (budget + 1024) + 4 + 2560,
                      'strings_rx_tx_and_cached_body_moving_peak_bound_bytes': 3 * (budget + 1024) + 5 + 2560,
                      'sdk_source_sha256': {str(p.relative_to(args.idf_path)): hashlib.sha256(p.read_bytes()).hexdigest()
                                            for p in (client_path, utils_path, parser_path, parser_path.parent / 'http_parser.h')},
                      'cases': rows, 'network_accessed': False, 'device_accessed': False,
                      'r5_qualified': False, 'r6_qualified': False}, ensure_ascii=False, indent=2))
