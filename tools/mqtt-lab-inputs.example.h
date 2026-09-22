/* 本文件只展示编译输入格式。复制到仓外受限目录，填写本轮隔离实验服务。
 * 真实密码、CA 和地址只进入私有构建目录及固件，不提交。不要修改本示例为真实值。 */
static ebase_mqtt_config_t lab_mqtt_config = {
    .hostname = "broker.example.invalid",
    .port = 8883,
    .tls = true,
    .username = "integration-test",
    .password = "fixture-only",
    .ca_pem = "-----BEGIN CERTIFICATE-----\nREPLACE_WITH_LAB_CA\n-----END CERTIFICATE-----\n",
};
static const char lab_ntp_server[] = "time.example.invalid";
