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
#include "esp_log.h"

#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include "lwip/netdb.h"
#include "lwip/dns.h"

#include "picohttpparser.h"
#include "nano_rest.h"

char rx_string[RX_BUFFER_BYTES];

static const char *TAG = "network_rest";

// Can be set via the setter functions
static volatile char *remote_domain = NULL;
static volatile uint16_t remote_port = 0;
static volatile char *remote_path = NULL;

void nano_rest_set_remote_domain(char *str){
    if( NULL != remote_domain ){
        free(remote_domain);
    }
    if( NULL != str ){
        remote_domain = malloc(strlen(str)+1);
        strcpy(remote_domain, str);
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
        free(remote_path);
    }
    if( NULL != str ){
        remote_path = malloc(strlen(str)+1);
        strcpy(remote_path, str);
    }
    else{
        remote_path = NULL;
    }
}

static void sleep_function(int milliseconds) {
    vTaskDelay(milliseconds / portTICK_PERIOD_MS);
}

static char *http_request_task(int get_post, unsigned char *post_data,
        unsigned char *result_data_buf, size_t result_data_buf_len) {
    int s, r;
    char request_packet[256];
    if (get_post == 0) {
        snprintf(request_packet, 256, "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: esp-idf/1.0 esp32\r\n\r\n", remote_path, remote_domain);
    }
    else if (get_post == 1) {
        size_t post_data_length = strlen((const char*)post_data);
        // todo: possibility that this could be truncated
        snprintf(request_packet, 256, "POST %s HTTP/1.0\r\n"
                 "Host: %s\r\n" \
                 "User-Agent: esp-idf/1.0 esp32\r\n"
                 "Content-Type: text/plain\r\n"
                 "Content-Length: %d\r\n"
                 "\r\n"
                 "%s"
                 , remote_path, remote_domain, post_data_length, post_data);
        ESP_LOGI(TAG, "POST Request Packet:\n%s", request_packet);
    }
    else {
        ESP_LOGE(TAG, "Error, POST/Get not selected");
        return;
    }
    struct addrinfo *res;
    struct in_addr *addr;
    {
        const struct addrinfo hints = {
            .ai_family = AF_INET,
            .ai_socktype = SOCK_STREAM,
        };
        ESP_LOGI(TAG, "Performing DNS lookup");
        ESP_LOGI(TAG, "Remote Domain: %s", remote_domain);
        char port[10];
        snprintf(port, sizeof(port), "%d", remote_port);
        ESP_LOGI(TAG, "Remote Port: %s", port);
        int err = getaddrinfo(remote_domain, port, &hints, &res);
        ESP_LOGI(TAG, "DNS lookup success");
        
        if(err != 0 || res == NULL) {
            ESP_LOGE(TAG, "DNS lookup failed err=%d res=%p", err, res);
            return;
        }
        else {
            ESP_LOGI(TAG, "DNS lookup success");
        }
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
        char recv_buf[256] = { 0 };
        r = read(s, recv_buf, sizeof(recv_buf)-1);
        for(int i = 0; i < r; i++) {
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
    ESP_LOGI(TAG, "Message Size: %d", msg_size);

    if(result_data_buf_len > msg_size) {
        strncpy((char *)result_data_buf, (char *)&newbuf[ret], msg_size);
        result_data_buf[msg_size] = '\0';
    }
    else {
        strlcpy((char *)result_data_buf, (char *)&newbuf[ret], result_data_buf_len);
    }
    if(result_data_buf_len > msg_size) { // todo: this may be off by one
    }
    ESP_LOGI(TAG, "phr_parse_response:\n%s", (char *) result_data_buf);
    close(s);
    return (char *) result_data_buf;
}



int network_get_data(unsigned char *user_rpc_command,
        unsigned char *result_data_buf, size_t result_data_buf_len){
    http_request_task(1, user_rpc_command, result_data_buf, result_data_buf_len);
    return 0;
}
