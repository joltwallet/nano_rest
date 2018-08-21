/* nanolws - libwebsocket wrapper
Copyright (C) 2018  Brian Pugh, James Coxon, Michael Smaili
https://www.joltwallet.com/
*/

#ifndef __INCLUDE_REST_H__
#define __INCLUDE_REST_H__

#define RX_BUFFER_BYTES (1536)
#define RECEIVE_POLLING_PERIOD_MS pdMS_TO_TICKS(10000)

int network_get_data(unsigned char *user_rpc_command, 
        unsigned char *result_data_buf, size_t result_data_buf_len);
void network_task(void *pvParameters);

void nano_rest_set_remote_domain(char *str);
void nano_rest_set_remote_port(uint16_t port);
void nano_rest_set_remote_path(char *str);

#endif
