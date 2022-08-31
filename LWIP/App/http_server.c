/*
 * HTTP server example.
 *
 * This sample code is in the public domain.
 */
#include <string.h>
#include <stdio.h>
#include <FreeRTOS.h>
#include <task.h>
#include <httpd.h>
#include "stdbool.h"
#include "tcp.h"

typedef struct
{
	GPIO_TypeDef* GPIOx;
	uint16_t GPIO_Pin;
}infoLeds_t;

static const infoLeds_t infoLeds[] =
{
	{
		LD1_GPIO_Port, LD1_Pin
	},
	{
		LD2_GPIO_Port, LD2_Pin
	},
	{
		LD3_GPIO_Port, LD3_Pin
	},
};

#define TOTAL_LEDS	(sizeof(infoLeds) / sizeof(infoLeds[0]))

typedef enum
{
	ID_LED_1 = 0,
	ID_LED_2,
	ID_LED_3,
}idLed_t;

#define TEXT_LENGTH		16

static char strText[TEXT_LENGTH];

static bool getLed(uint8_t idLed)
{
	bool ret = false;

	if (idLed < TOTAL_LEDS)
		ret = !HAL_GPIO_ReadPin(infoLeds[idLed].GPIOx, infoLeds[idLed].GPIO_Pin);

	return ret;
}

static void setLed(uint8_t idLed, bool est)
{
	HAL_GPIO_WritePin(infoLeds[idLed].GPIOx, infoLeds[idLed].GPIO_Pin, !est);
}

static void toggleLed(uint8_t idLed)
{
	HAL_GPIO_TogglePin(infoLeds[idLed].GPIOx, infoLeds[idLed].GPIO_Pin);
}

enum {
    SSI_UPTIME,
    SSI_FREE_HEAP,
    SSI_LED_STATE
};

int32_t ssi_handler(int32_t iIndex, char *pcInsert, int32_t iInsertLen)
{
    switch (iIndex) {
        case SSI_UPTIME:
            snprintf(pcInsert, iInsertLen, "%d",
                    (int)(xTaskGetTickCount() * portTICK_PERIOD_MS / 1000));
            break;
        case SSI_FREE_HEAP:
            snprintf(pcInsert, iInsertLen, "%d", (int) xPortGetFreeHeapSize());
            break;
        case SSI_LED_STATE:
            snprintf(pcInsert, iInsertLen, getLed(ID_LED_2) ? "Off" : "On");
            break;
        default:
            snprintf(pcInsert, iInsertLen, "N/A");
            break;
    }

    /* Tell the server how many characters to insert */
    return (strlen(pcInsert));
}

const char *gpio_cgi_handler(int iIndex, int iNumParams, char *pcParam[], char *pcValue[])
{
    for (int i = 0; i < iNumParams; i++) {
        if (strcmp(pcParam[i], "on") == 0) {
            uint8_t gpio_num = atoi(pcValue[i]);
            setLed(gpio_num - 1, true);
        } else if (strcmp(pcParam[i], "off") == 0) {
            uint8_t gpio_num = atoi(pcValue[i]);
            setLed(gpio_num - 1, false);
        } else if (strcmp(pcParam[i], "toggle") == 0) {
            uint8_t gpio_num = atoi(pcValue[i]);
            toggleLed(gpio_num - 1);
		} else if (strcmp(pcParam[i], "button") == 0) {
			strncpy(strText, pcValue[i], sizeof(strText));
		}
    }
    return "/index.ssi";
}

const char *about_cgi_handler(int iIndex, int iNumParams, char *pcParam[], char *pcValue[])
{
    return "/about.html";
}

const char *websocket_cgi_handler(int iIndex, int iNumParams, char *pcParam[], char *pcValue[])
{
    return "/websockets.html";
}

void websocket_task(void *pvParameter)
{
    struct tcp_pcb *pcb = (struct tcp_pcb *) pvParameter;

    for (;;) {
        if (pcb == NULL || pcb->state != ESTABLISHED) {
            //printf("Connection closed, deleting task\n");
            break;
        }

        int uptime = xTaskGetTickCount() * portTICK_PERIOD_MS / 1000;
        int heap = (int) xPortGetFreeHeapSize();
        int led = !getLed(ID_LED_2);

        /* Generate response in JSON format */
        char response[64];
        int len = snprintf(response, sizeof (response),
                "{\"uptime\" : \"%d\","
                " \"heap\" : \"%d\","
                " \"led\" : \"%d\"}", uptime, heap, led);
        if (len < sizeof (response))
            websocket_write(pcb, (unsigned char *) response, len, WS_TEXT_MODE);

        vTaskDelay(2000 / portTICK_PERIOD_MS);
    }

    vTaskDelete(NULL);
}

/**
 * This function is called when websocket frame is received.
 *
 * Note: this function is executed on TCP thread and should return as soon
 * as possible.
 */
void websocket_cb(struct tcp_pcb *pcb, uint8_t *data, u16_t data_len, uint8_t mode)
{
//    printf("[websocket_callback]:\n%.*s\n", (int) data_len, (char*) data);

    uint8_t response[2];
    uint16_t val;

    switch (data[0]) {
        case 'A': // ADC
            /* This should be done on a separate thread in 'real' applications */
            val = xTaskGetTickCount() % 1024 ;
            break;
        case 'D': // Disable LED
        	setLed(ID_LED_2, true);
            val = 0xDEAD;
            break;
        case 'E': // Enable LED
        	setLed(ID_LED_2, false);
            val = 0xBEEF;
            break;
        default:
  //          printf("Unknown command\n");
            val = 0;
            break;
    }

    response[1] = (uint8_t) val;
    response[0] = val >> 8;

    websocket_write(pcb, response, 2, WS_BIN_MODE);
}

/**
 * This function is called when new websocket is open and
 * creates a new websocket_task if requested URI equals '/stream'.
 */
void websocket_open_cb(struct tcp_pcb *pcb, const char *uri)
{
//    printf("WS URI: %s\n", uri);
    if (!strcmp(uri, "/stream")) {
  //      printf("request for streaming\n");
        xTaskCreate(&websocket_task, "websocket_task", 256, (void *) pcb, 2, NULL);
    }
}

void httpd_task(void *pvParameters)
{
    static const tCGI pCGIs[] = {
        {"/gpio", (tCGIHandler) gpio_cgi_handler},
        {"/about", (tCGIHandler) about_cgi_handler},
        {"/websockets", (tCGIHandler) websocket_cgi_handler},
    };

    static const char *pcConfigSSITags[] = {
        "uptime", // SSI_UPTIME
        "heap",   // SSI_FREE_HEAP
        "led"     // SSI_LED_STATE
    };

    /* register handlers and start the server */
    http_set_cgi_handlers(pCGIs, sizeof (pCGIs) / sizeof (pCGIs[0]));
    http_set_ssi_handler((tSSIHandler) ssi_handler, pcConfigSSITags,
            sizeof (pcConfigSSITags) / sizeof (pcConfigSSITags[0]));
    websocket_register_callbacks((tWsOpenHandler) websocket_open_cb,
            (tWsHandler) websocket_cb);
    httpd_init();

    vTaskDelete(NULL);

    for (;;);
}

void http_server_init(void)
{
    /* initialize tasks */
    xTaskCreate(&httpd_task, "HTTP Daemon", 128, NULL, 2, NULL);
}
