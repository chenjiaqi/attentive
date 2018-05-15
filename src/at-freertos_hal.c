/*
 * Copyright © 2014 Kosma Moczek <kosma@cloudyourcar.com>
 * This program is free software. It comes without any warranty, to the extent
 * permitted by applicable law. You can redistribute it and/or modify it under
 * the terms of the Do What The Fuck You Want To Public License, Version 2, as
 * published by Sam Hocevar. See the COPYING file for more details.
 */

#include <attentive/at.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#define printf(...)
#include "debug.h"
#include "uart.h"

/* Defines -------------------------------------------------------------------*/
//DBG_SET_LEVEL(DBG_LEVEL_I);

// Remove once you refactor this out.
#define AT_COMMAND_LENGTH 80

struct at_freertos {
    struct at at;
    int timeout;
    char response[AT_BUF_SIZE];

    TaskHandle_t xTask;
    SemaphoreHandle_t xSem;
    hal_uart_t * xUART;

    bool running : 1;       /**< Reader thread should be running. */
    bool open : 1;          /**< FD is valid. Set/cleared by open()/close(). */
    bool busy : 1;          /**< FD is in use. Set/cleared by reader thread. */
    bool waiting : 1;       /**< Waiting for response callback to arrive. */
};

void at_reader_thread(void *arg);

static void handle_response(const char *buf, size_t len, void *arg)
{
    struct at_freertos *priv = (struct at_freertos *) arg;

    /* The mutex is held by the reader thread; don't reacquire. */
    len = len < AT_BUF_SIZE - 1 ? len : AT_BUF_SIZE - 1;
    memcpy(priv->response, buf, len);
    priv->response[len] = '\0';
    priv->waiting = false;
    xSemaphoreGive(priv->xSem);
    
}

static void handle_urc(const char *buf, size_t len, void *arg)
{
    struct at *at = (struct at *) arg;

    /* Forward to caller's URC callback, if any. */
    if (at->cbs->handle_urc)
        at->cbs->handle_urc(buf, len, at->arg);
}

enum at_response_type scan_line(const char *line, size_t len, void *arg)
{
    struct at *at = (struct at *) arg;

    enum at_response_type type = AT_RESPONSE_UNKNOWN;
    if (at->command_scanner)
        type = at->command_scanner(line, len, at->arg);
    if (!type && at->cbs && at->cbs->scan_line)
        type = at->cbs->scan_line(line, len, at->arg);
    return type;
}

static const struct at_parser_callbacks parser_callbacks = {
    .handle_response = handle_response,
    .handle_urc = handle_urc,
    .scan_line = scan_line,
};

struct at *at_alloc_freertos(hal_uart_t *p_uart)
{
    static struct at_freertos at;
    struct at_freertos *priv = &at;
    
    memset(priv, 0, sizeof(struct at_freertos));

    /* allocate underlying parser */
    priv->at.parser = at_parser_alloc(&parser_callbacks, (void *) priv);
    priv->xUART = p_uart;

    /* initialize and start reader thread */
    priv->running = true;
    /*priv->xMutex = xSemaphoreCreateBinary();*/
    // CAUSING: create the reader task at high priority
    priv->xSem = xSemaphoreCreateBinary();
    if(!priv->xSem)
    {
        DBG_E("xSem create error");
    }

    //xTaskCreate(at_reader_thread, "ATReadTask", configMINIMAL_STACK_SIZE * 2, priv, 4, &priv->xTask);
    xTaskCreate(at_reader_thread, "ATReadTask", 384, priv, 5, &priv->xTask);

    return (struct at *) priv;
}

int at_open(struct at *at)
{
    DBG_I(__FUNCTION__);
    struct at_freertos *priv = (struct at_freertos *) at;

    //priv->xUART = hal_uart_get_instance(0);
    if(priv->xUART == NULL) {
        DBG_E("xUART is NULL ");
        return -1;
    } else {
        priv->xUART->ops->set_rx_enable(priv->xUART,true);
    }

    priv->open = true;
    
    xSemaphoreTake(priv->xSem, 0);
    return 0;
}

int at_close(struct at *at)
{
    struct at_freertos *priv = (struct at_freertos *) at;

    /* Mark the port descriptor as invalid. */
    priv->open = false;

    //FreeRTOS_close(priv->xUART);
    priv->xUART->ops->deinit(priv->xUART);
    priv->xUART = NULL;

    return 0;
}

void at_free(struct at *at)
{
    struct at_freertos *priv = (struct at_freertos *) at;

    /* make sure the channel is closed */
    at_close(at);

    /* ask the reader thread to terminate */
    priv->running = false;
    if(priv->xTask != NULL) {
        vTaskDelete(priv->xTask);
    }
    /* delete the semaphore */
    if(priv->xSem != NULL) {
        vSemaphoreDelete(priv->xSem);
    }
}

int at_suspend(struct at *at)
{
    struct at_freertos *priv = (struct at_freertos *) at;

    if(priv != NULL && priv->xTask != NULL) {
        vTaskSuspend(priv->xTask);
    }

    return 0;
}

int at_resume(struct at *at)
{
    struct at_freertos *priv = (struct at_freertos *) at;

    if(priv != NULL && priv->xTask != NULL) {
        vTaskResume(priv->xTask);
    }

    return 0;
}

void at_set_callbacks(struct at *at, const struct at_callbacks *cbs, void *arg)
{
    at->cbs = cbs;
    at->arg = arg;
}

void at_set_command_scanner(struct at *at, at_line_scanner_t scanner)
{
    at->command_scanner = scanner;
}

void at_set_timeout(struct at *at, int timeout)
{
    struct at_freertos *priv = (struct at_freertos *) at;

    priv->timeout = timeout;
}

void at_set_character_handler(struct at *at, at_character_handler_t handler)
{
    at_parser_set_character_handler(at->parser, handler);
}

void at_expect_dataprompt(struct at *at, const char *prompt)
{
    at_parser_expect_dataprompt(at->parser, prompt);
}

static const char *_at_command(struct at_freertos *priv, const void *data, size_t size)
{
    /*if(!xSemaphoreTake(priv->xMutex, pdMS_TO_TICKS(1000))) {*/
        /*return NULL;*/
    /*}*/

    /* Bail out if the channel is closing or closed. */
    if (!priv->open) {
        /*xSemaphoreGive(priv->xMutex);*/
        return NULL;
    }

    /* Prepare parser. */
    at_parser_await_response(priv->at.parser);
    //DBG_I("parser state %d",(priv->at.parser)->state);

    /* Send the command. */
    // FIXME: handle interrupts, short writes, errors, etc.

    /**TODO:*/
    //FreeRTOS_write(priv->xUART, data, size);
    
    
    priv->xUART->ops->write(priv->xUART,data,size);
    //priv->xUART->ops->set_rx_enable(priv->xUART,true);

    
    /* Wait for the parser thread to collect a response. */
    priv->waiting = true;
    /*xSemaphoreGive(priv->xMutex);*/
    xSemaphoreTake(priv->xSem, 0);
    xSemaphoreTake(priv->xSem, 0);
    //xSemaphoreTake(priv->xSem, 0);
    int timeout = priv->timeout;
    while (timeout-- && priv->open && priv->waiting) {

        if (xSemaphoreTake(priv->xSem, pdMS_TO_TICKS(1000)) == pdTRUE) {
            break;
        }
    }

    /*xSemaphoreTake(priv->xMutex, pdMS_TO_TICKS(1000));*/
    const char *result;
    if (!priv->open) {
        /* The serial port was closed behind our back. */
        DBG_E("PRIC is NULL")
        result = NULL;
    } else if (priv->waiting) {
        /* Timed out waiting for a response. */
        DBG_I("WAITTING");
        at_parser_reset(priv->at.parser);
        result = NULL;
    } else {
        /* Response arrived. */
        result = priv->response;
    }

    /* Reset per-command settings. */
    priv->at.command_scanner = NULL;
    /*xSemaphoreGive(priv->xMutex);*/

    return result;
}

const char *at_command(struct at *at, const char *format, ...)
{
    //DBG_I(__FUNCTION__);
    struct at_freertos *priv = (struct at_freertos *) at;

    /* Build command string. */
    va_list ap;
    va_start(ap, format);
    char line[AT_COMMAND_LENGTH];
    int len = vsnprintf(line, sizeof(line)-1, format, ap);
    va_end(ap);

    /* Bail out if we run out of space. */
    if (len >= (int)(sizeof(line)-1)) {
        return NULL;
    }

    DBG_V("<< %s\r\n", line);

    /* Append modem-style newline. */
    line[len++] = '\r';

    /* Send the command. */
    return _at_command(priv, line, len);
}

const char *at_command_raw(struct at *at, const void *data, size_t size)
{
    struct at_freertos *priv = (struct at_freertos *) at;

    DBG_V("<< [%d bytes]\n", size);

    return _at_command(priv, data, size);
}

bool _at_send(struct at_freertos *priv, const void *data, size_t size)
{
    /* Bail out if the channel is closing or closed. */
    if (!priv->open) {
        /*xSemaphoreGive(priv->xMutex);*/
        return false;
    }

    /* Send the command. */
    // FIXME: handle interrupts, short writes, errors, etc.
    //return FreeRTOS_write(priv->xUART, data, size) == size;
    return priv->xUART->ops->write(priv->xUART,data,size);
}

bool at_send(struct at *at, const char *format, ...)
{
    struct at_freertos *priv = (struct at_freertos *) at;

    /* Build a string. */
    va_list ap;
    va_start(ap, format);
    char line[AT_COMMAND_LENGTH];
    int len = vsnprintf(line, sizeof(line)-1, format, ap);
    va_end(ap);

    /* Bail out if we run out of space. */
    if (len >= (int)(sizeof(line)-1)) {
        return false;
    }

    DBG_V("S< %s\n", line);

    /* Send the string. */
    return _at_send(priv, line, len);
}

bool at_send_raw(struct at *at, const void *data, size_t size)
{
    struct at_freertos *priv = (struct at_freertos *) at;

    DBG_V("R< [%d bytes]\n", size);

    return _at_send(priv, data, size);
}

inline void byte_to_hex(char byte, char* hex) {
    int h = byte & 0x0F;
    hex[1] = h < 10 ? '0' + h : 'A' + h - 10;

    h = (byte >> 4) & 0x0F;
    hex[0] = h < 10 ? '0' + h : 'A' + h - 10;
}

bool at_send_hex(struct at *at, const void *data, size_t size)
{
    struct at_freertos *priv = (struct at_freertos *) at;

    DBG_V("H< [%d bytes]\n", size);

    int oset = 0;
    while(oset < size) {
        char line[AT_COMMAND_LENGTH];

        int len = sizeof(line) / 2;
        len = size - oset > len ? len : size - oset;

        for(int i = 0; i < len; i++) {
            byte_to_hex(((char*)data)[oset + i], &line[i * 2]);
        }
        oset += len;
        if(!_at_send(priv, line, len * 2)) {
            return false;
        }
    }

    return true;
}

int at_config(struct at *at, const char *option, const char *value, int attempts)
{
    for (int i = 0; i < attempts; i++) {
        /* Blindly try to set the configuration option. */
        at_command(at, "AT+%s=%s", option, value);

        /* Query the setting status. */
        const char *response = at_command(at, "AT+%s?", option);
        /* Bail out on timeouts. */
        if (response == NULL) {
            return -2;
        }
        /* Check if the setting has the correct value. */
        char expected[32];
        if (snprintf(expected, sizeof(expected), "+%s: %s", option, value) >= (int) sizeof(expected)) {
            return -1;
        }
        if (!strncmp(response, expected, strlen(expected))) {
            return 0;
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    return 0;
}

void at_reader_thread(void *arg)
{
    struct at_freertos *priv = (struct at_freertos *)arg;

    while (true) {
        if (!priv->running) {

            DBG_W("Not running");
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        /* Wait for the port descriptor to be valid. */
        else if(!priv->open) {
            
            //DBG_W("Not open");
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        //DBG_I(__FUNCTION__);

        /* Lock access to the port descriptor. */
        /*xSemaphoreTake(priv->xMutex, pdMS_TO_TICKS(1000));*/
        priv->busy = true;

        /* Attempt to read some data. */
        char ch;
        //int result = FreeRTOS_read(priv->xUART, &ch, 1);

        //priv->xUART->ops->set_rx_enable(priv->xUART,true);
        int result = priv->xUART->ops->read(priv->xUART, &ch, 1);
        

        /* Unlock access to the port descriptor. */
        priv->busy = false;
        /* Notify at_close() that the port is now free. */

        if (result == 1) {
            /* Data received, feed the parser. */
            at_parser_feed(priv->at.parser, &ch, 1);
        }
        /*xSemaphoreGive(priv->xMutex);*/
    }

}

/* vim: set ts=4 sw=4 et: */
