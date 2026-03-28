#ifndef WEB_CONFIG_H
#define WEB_CONFIG_H

int web_config_start(const char *config_path);
void web_config_stop(void);
void web_config_set_gateway_status(int enabled, int connected,
								   const char *progid, const char *message);
void web_config_set_opcua_status(int enabled, int listening, int port, const char *message);
void web_config_set_gateway_runtime_enabled(int enabled);
int web_config_get_gateway_runtime_enabled(void);

#endif /* WEB_CONFIG_H */
