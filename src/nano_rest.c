/* nano_rest - restful wrapper
 Copyright (C) 2018  Brian Pugh, James Coxon, Michael Smaili
 https://www.joltwallet.com/
 */

#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include "esp_event_loop.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include "lwip/netdb.h"
#include "lwip/dns.h"

#include "picohttpparser.h"

#include "esp_log.h"

#include "nano_rest.h"

/* Constants that aren't configurable in menuconfig */
#define WEB_SERVER "yapraiwallet.space"
#define WEB_PORT "5000"
#define WEB_URL "/work"

char rx_string[RX_BUFFER_BYTES];
unsigned char *rpc_command = &buf[RX_BUFFER_BYTES];

static const char *TAG = "network_task";

// Can be set via the setter functions
static volatile char *remote_domain = NULL;
static volatile uint16_t remote_port = 0;
static volatile char *remote_path = NULL;

void nano_rest_set_remote_domain(char *str){
    if( NULL != remote_domain ){
        free((char *)remote_domain);
    }
    if( NULL != str ){
        remote_domain = malloc(strlen(str)+1);
        strcpy((char *)remote_domain, str);
    }
    else{
        remote_domain = NULL;
    }
}

void nano_rest_set_remote_port(uint16_t port){
    remote_port = port;
}

void nano_rest_set_remote_path(char *str){
    if( NULL != remote_path ){
        free((char *)remote_path);
    }
    if( NULL != str ){
        remote_path = malloc(strlen(str)+1);
        strcpy((char *)remote_path, str);
    }
    else{
        remote_path = NULL;
    }
}

void sleep_function(int milliseconds) {
    vTaskDelay(milliseconds / portTICK_PERIOD_MS);
}

static esp_err_t event_handler(void *ctx, system_event_t *event)
{
    switch(event->event_id) {
        case SYSTEM_EVENT_STA_START:
            esp_wifi_connect();
            break;
        case SYSTEM_EVENT_STA_GOT_IP:
            xEventGroupSetBits(wifi_event_group, CONNECTED_BIT);
            break;
        case SYSTEM_EVENT_STA_DISCONNECTED:
            /* This is a workaround as ESP32 WiFi libs don't currently
             auto-reassociate. */
            esp_wifi_connect();
            xEventGroupClearBits(wifi_event_group, CONNECTED_BIT);
            break;
        default:
            break;
    }
    return ESP_OK;
}

char http_request_task(char *web_url, char *web_server, int get_post, char *post_data, unsigned char *result_data_buf, size_t result_data_buf_len)
{
    char request_packet[256];
    if (get_post == 0) {
        snprintf(request_packet, 256, "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: esp-idf/1.0 esp32\r\n\r\n", web_url, web_server);
    }
    else if (get_post == 1) {
        size_t post_data_length = strlen(post_data);
        
        snprintf(request_packet, 256, "POST %s HTTP/1.0\r\n"
                 "Host: %s\r\n" \
                 "User-Agent: esp-idf/1.0 esp32\r\n"
                 "Content-Type: text/plain\r\n"
                 "Content-Length: %d\r\n"
                 "\r\n"
                 "%s"
                 , web_url, web_server, post_data_length, post_data);
        ESP_LOGE(TAG, "%s", request_packet);
    }
    else {
        ESP_LOGE(TAG, "Error, POST/Get not selected");
        return;
    }
    
    const struct addrinfo hints = {
        .ai_family = AF_INET,
        .ai_socktype = SOCK_STREAM,
    };
    struct addrinfo *res;
    struct in_addr *addr;
    int s, r;
    char recv_buf[256];
    
    ESP_LOGI(TAG, "Connected to AP");
    
    int err = getaddrinfo(WEB_SERVER, WEB_PORT, &hints, &res);
    
    if(err != 0 || res == NULL) {
        ESP_LOGE(TAG, "DNS lookup failed err=%d res=%p", err, res);
        return;
    }
    
    /* Code to print the resolved IP.
     
     Note: inet_ntoa is non-reentrant, look at ipaddr_ntoa_r for "real" code */
    addr = &((struct sockaddr_in *)res->ai_addr)->sin_addr;
    ESP_LOGI(TAG, "DNS lookup succeeded. IP=%s", inet_ntoa(*addr));
    
    s = socket(res->ai_family, res->ai_socktype, 0);
    if(s < 0) {
        ESP_LOGE(TAG, "... Failed to allocate socket.");
        freeaddrinfo(res);
        return;
    }
    ESP_LOGI(TAG, "... allocated socket");
    
    if(connect(s, res->ai_addr, res->ai_addrlen) != 0) {
        ESP_LOGE(TAG, "... socket connect failed errno=%d", errno);
        close(s);
        freeaddrinfo(res);
        return;
    }
    
    ESP_LOGI(TAG, "... connected");
    freeaddrinfo(res);
    
    if (write(s, request_packet, strlen(request_packet)) < 0) {
        ESP_LOGE(TAG, "... socket send failed");
        close(s);
        return;
    }
    ESP_LOGI(TAG, "... socket send success");
    
    struct timeval receiving_timeout;
    receiving_timeout.tv_sec = 5;
    receiving_timeout.tv_usec = 0;
    if (setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &receiving_timeout,
                   sizeof(receiving_timeout)) < 0) {
        ESP_LOGE(TAG, "... failed to set socket receiving timeout");
        close(s);
        return;
    }
    ESP_LOGI(TAG, "... set socket receiving timeout success");
    
    char newbuf[256];
    int y = 0;
    /* Read HTTP response */
    do {
        bzero(recv_buf, sizeof(recv_buf));
        r = read(s, recv_buf, sizeof(recv_buf)-1);
        for(int i = 0; i < r; i++) {
            //putchar(recv_buf[i]);
            newbuf[y] = recv_buf[i];
            y++;
        }
    } while(r > 0);
    
    ESP_LOGI(TAG, "... done reading from socket. Last read return=%d errno=%d\r\n", r, errno);
    
    int ret, minor_version, status;
    struct phr_header headers[100];
    const char* msg;
    size_t msg_len, num_headers;
    
    num_headers = sizeof(headers) / sizeof(headers[0]);
    ret = phr_parse_response(newbuf, strlen(newbuf), &minor_version, &status, &msg,
                             &msg_len, headers, &num_headers, 0);
    
    int msg_size = y - ret;
    char message[msg_size];
    memcpy(message, &newbuf[ret], ret * sizeof(int));
    printf("phr_parse_response:\n%s\n", message);
    
    close(s);
    return message;
}



int network_get_data(unsigned char *user_rpc_command,
        unsigned char *result_data_buf, size_t result_data_buf_len){
  
    http_request_task("/api", "yapraiwallet.space", 1, user_rpc_command, result_data_buf, result_data_buf_len);

    
    return 0;
}
