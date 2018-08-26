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

static const char GET_FORMAT_STR[] = \
        "GET %s HTTP/1.0\r\n"
        "Host: %s\r\n"
        "User-Agent: esp-idf/1.0 esp32\r\n"
        "\r\n";

static const char POST_FORMAT_STR[] = \
        "POST %s HTTP/1.0\r\n"
         "Host: %s\r\n" \
         "User-Agent: esp-idf/1.0 esp32\r\n"
         "Content-Type: text/plain\r\n"
         "Content-Length: %d\r\n"
         "\r\n"
         "%s";

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

static char *http_request_task(int get_post, char *post_data,
        char *result_data_buf, size_t result_data_buf_len) {
    int s = -1; // socket descriptor
    int r;
    char *request_packet = NULL;
    struct addrinfo *addrinfo = NULL;
    char * func_result = NULL;
    char *http_response = NULL;
    char *http_response_new = NULL;
    int http_response_len = 0;

    if( 0 == get_post) {
        size_t request_packet_len = strlen(GET_FORMAT_STR) + 
                strlen(remote_path) + strlen(remote_domain) + 1;
        request_packet = malloc( request_packet_len );
        snprintf(request_packet, request_packet_len, GET_FORMAT_STR,
                remote_path, remote_domain);
    }
    else if ( 1 == get_post ) {
        // 5 is for the uint16 port
        size_t request_packet_len = strlen(POST_FORMAT_STR) + 
                strlen(remote_path) + strlen(remote_domain) +
                strlen(post_data) + 5 + 1;
        request_packet = malloc( request_packet_len );

        size_t post_data_length = strlen((const char*)post_data);
        // todo: possibility that this could be truncated
        snprintf(request_packet, request_packet_len, POST_FORMAT_STR,
                 remote_path, remote_domain, post_data_length, post_data);
        ESP_LOGI(TAG, "POST Request Packet:\n%s", request_packet);
    }
    else {
        ESP_LOGE(TAG, "Error, POST/Get not selected");
        goto exit;
    }
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
        int err = getaddrinfo(remote_domain, port, &hints, &addrinfo);
        ESP_LOGI(TAG, "DNS lookup success");
        
        if(err != 0 || addrinfo == NULL) {
            ESP_LOGE(TAG, "DNS lookup failed err=%d addrinfo=%p", err, addrinfo);
            goto exit;
        }
        else {
            ESP_LOGI(TAG, "DNS lookup success");
        }
    }
    
    /* Code to print the resolved IP.
     Note: inet_ntoa is non-reentrant, look at ipaddr_ntoa_r for "real" code */
    {
        struct in_addr *addr;
        addr = &((struct sockaddr_in *)addrinfo->ai_addr)->sin_addr;
        ESP_LOGI(TAG, "DNS lookup succeeded. IP=%s", inet_ntoa(*addr));
    }
    
    /* Open Socket Connection */
    s = socket(addrinfo->ai_family, addrinfo->ai_socktype, 0);
    if( s < 0 ) {
        ESP_LOGE(TAG, "... Failed to allocate socket.");
        goto exit;
    }
    ESP_LOGI(TAG, "... allocated socket");
    if( 0 != connect(s, addrinfo->ai_addr, addrinfo->ai_addrlen) ) {
        ESP_LOGE(TAG, "... socket connect failed errno=%d", errno);
        goto exit;
    }
    ESP_LOGI(TAG, "... connected");
    freeaddrinfo(addrinfo);
    addrinfo = NULL;
    
    /* Write Request to Socket */
    if (write(s, request_packet, strlen(request_packet)) < 0) {
        ESP_LOGE(TAG, "... socket send failed");
        goto exit;
    }
    ESP_LOGI(TAG, "... socket send success");
    
    {
        struct timeval receiving_timeout;
        receiving_timeout.tv_sec = CONFIG_NANO_REST_RECEIVE_TIMEOUT;
        receiving_timeout.tv_usec = 0;
        if (setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &receiving_timeout,
                       sizeof(receiving_timeout)) < 0) {
            ESP_LOGE(TAG, "... failed to set socket receiving timeout");
            goto exit;
        }
        ESP_LOGI(TAG, "... set socket receiving timeout success");
    }

    /* Read HTTP response */
    do {
        http_response_new = realloc(http_response, http_response_len + CONFIG_NANO_REST_RECEIVE_BLOCK_SIZE);
        if( NULL == http_response_new ) {
            ESP_LOGE(TAG, "Unable to allocate additional memory for http_response");
            goto exit;
        }
        else {
            http_response = http_response_new;
        }
        char recv_buf[CONFIG_NANO_REST_RECEIVE_BLOCK_SIZE] = { 0 };
        r = read(s, recv_buf, sizeof(recv_buf)-1);
        memcpy(&http_response[http_response_len], recv_buf, r);
        http_response_len += r;
    } while( r == CONFIG_NANO_REST_RECEIVE_BLOCK_SIZE - 1 );
    ESP_LOGI(TAG, "... done reading from socket. Last read return=%d errno=%d\r\n", r, errno);
   
    {
        int ret, minor_version, status;
        struct phr_header headers[100];
        const char* msg;
        size_t msg_len, num_headers;
        
        num_headers = sizeof(headers) / sizeof(headers[0]);
        ret = phr_parse_response(http_response, strlen(http_response), 
                &minor_version, &status, 
                &msg, &msg_len,
                headers, &num_headers, 0);
        int msg_size = http_response_len - ret;
        ESP_LOGI(TAG, "Message Size: %d", msg_size);

        if(result_data_buf_len > msg_size) {
            strncpy((char *)result_data_buf, (char *)&http_response[ret], msg_size);
            result_data_buf[msg_size] = '\0';
        }
        else {
            ESP_LOGE(TAG, "Insufficient result buffer.");
            goto exit;
        }
        ESP_LOGI(TAG, "phr_parse_response:\n%s", (char *) result_data_buf);
    }
    func_result = result_data_buf;
exit:
    if( addrinfo ) {
        freeaddrinfo(addrinfo);
    }
    if( request_packet ) {
        free(request_packet);
    }
    if( s >= 0 ) {
        close(s);
    }
    if( http_response ) {
        free(http_response);
    }
    return func_result;
}

int network_get_data(char *post_data,
        char *result_data_buf, size_t result_data_buf_len){
    http_request_task(1, post_data, result_data_buf, result_data_buf_len);
    return 0;
}
