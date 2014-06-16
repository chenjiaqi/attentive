#include "at_parser.h"

#include <stdio.h>
#include <string.h>

enum at_parser_state {
    STATE_IDLE,
    STATE_READLINE,
    STATE_DATAPROMPT,
    STATE_RAWDATA,
    STATE_HEXDATA,
};

struct at_parser {
    const struct at_parser_callbacks *cbs;
    void *priv;
    line_scanner_t per_command_scanner;

    enum at_parser_state state;
    size_t data_left;
    int nibble;

    char *buf;
    size_t buf_used;
    size_t buf_size;
    size_t buf_current;
};

static const char *ok_responses[] = {
    "OK",
    "> ",
    NULL
};

static const char *error_responses[] = {
    "ERROR",
    "NO CARRIER",
    "+CME ERROR:",
    "+CMS ERROR:",
    NULL,
};

static const char *urc_responses[] = {
    "RING",
    NULL,
};

struct at_parser *at_parser_alloc(const struct at_parser_callbacks *cbs, size_t bufsize, void *priv)
{
    /* Allocate parser struct. */
    struct at_parser *parser = (struct at_parser *) malloc(sizeof(struct at_parser));
    if (parser == NULL) {
        errno = ENOMEM;
        return NULL;
    }

    /* Allocate response buffer. */
    parser->buf = malloc(bufsize);
    if (parser->buf == NULL) {
        free(parser);
        errno = ENOMEM;
        return NULL;
    }
    parser->cbs = cbs;
    parser->buf_size = bufsize;
    parser->priv = priv;

    /* Prepare instance. */
    at_parser_reset(parser);

    return parser;
}

void at_parser_reset(struct at_parser *parser)
{
    parser->state = STATE_IDLE;
    parser->per_command_scanner = NULL;
    parser->buf_used = 0;
    parser->buf_current = 0;
    parser->data_left = 0;
}

void at_parser_await_response(struct at_parser *parser, bool dataprompt, line_scanner_t scanner)
{
    parser->per_command_scanner = scanner;
    parser->state = (dataprompt ? STATE_DATAPROMPT : STATE_READLINE);
}

bool at_prefix_in_table(const char *line, const char *table[])
{
    for (int i=0; table[i] != NULL; i++)
        if (!strncmp(line, table[i], strlen(table[i])))
            return true;

    return false;
}

static enum at_response_type generic_line_scanner(const char *line, size_t len)
{
    (void) len;

    if (at_prefix_in_table(line, urc_responses))
        return AT_RESPONSE_URC;
    else if (at_prefix_in_table(line, error_responses))
        return AT_RESPONSE_FINAL;
    else if (at_prefix_in_table(line, ok_responses))
        return AT_RESPONSE_FINAL_OK;
    else
        return AT_RESPONSE_INTERMEDIATE;
}

static void parser_append(struct at_parser *parser, char ch)
{
    if (parser->buf_used < parser->buf_size-1)
        parser->buf[parser->buf_used++] = ch;
}

/**
 * Helper, called whenever a full response line is collected.
 */
static void parser_handle_line(struct at_parser *parser)
{
    /* Skip empty lines. */
    if (parser->buf_used == parser->buf_current)
        return;

    /* NULL-terminate the response .*/
    parser->buf[parser->buf_used] = '\0';

    /* Extract line address & length for later use. */
    const char *line = parser->buf + parser->buf_current;
    size_t len = parser->buf_used - parser->buf_current;

    /* Log the received line. */
    printf("[%.*s] (%d)\n", (int) parser->buf_used, parser->buf, (int) parser->buf_used);
    printf("< '%.*s'\n", (int) len, line);

    /* Determine response type. */
    enum at_response_type type = AT_RESPONSE_UNKNOWN;
    if (parser->per_command_scanner)
        type = parser->per_command_scanner(line, len, parser->priv);
    if (!type && parser->cbs->scan_line)
        type = parser->cbs->scan_line(line, len, parser->priv);
    if (!type)
        type = generic_line_scanner(line, len);

    /* Expected URCs and all unexpected lines are sent to URC handler. */
    if (type == AT_RESPONSE_URC || parser->state == STATE_IDLE)
    {
        /* Fire the callback on the URC line. */
        parser->cbs->handle_urc(parser->buf + parser->buf_current,
                                parser->buf_used - parser->buf_current,
                                parser->priv);

        /* Discard the newline before the URC (if any). */
        if (parser->buf_current > 0)
            parser->buf_current--;

        /* Discard the URC line from the buffer. */
        parser->buf_used = parser->buf_current;

        return;
    }

    /* Accumulate everything that's not a final OK. */
    if (type != AT_RESPONSE_FINAL_OK) {
        /* Include the line in the buffer. */
        parser->buf_current = parser->buf_used;
    } else {
        /* Discard the newline before the OK (if any). */
        if (parser->buf_current > 0)
            parser->buf_current--;

        /* Discard the line from the buffer. */
        parser->buf_used = parser->buf_current;
        parser->buf[parser->buf_used] = '\0';
    }

    /* Act on the response type. */
    switch (type & _AT_RESPONSE_TYPE_MASK) {
        case AT_RESPONSE_FINAL_OK:
        case AT_RESPONSE_FINAL:
        {
            /* Fire the response callback. */
            parser->cbs->handle_response(parser->buf, parser->buf_used, parser->priv);

            /* Go back to idle state. */
            at_parser_reset(parser);
        }
        break;

        case _AT_RESPONSE_RAWDATA_FOLLOWS:
        {
            /* Switch parser state to rawdata mode. */
            parser->data_left = (int)type >> 8;
            parser->state = STATE_RAWDATA;
            printf("rawdata follows (%d bytes)\n", (int) parser->data_left);
        }
        break;

        case _AT_RESPONSE_HEXDATA_FOLLOWS:
        {
            /* Switch parser state to hexdata mode. */
            parser->data_left = (int)type >> 8;
            parser->nibble = -1;
            parser->state = STATE_HEXDATA;
        }
        break;

        default:
        {
            /* Keep calm and carry on. */
        }
        break;
    }
}

void at_parser_feed(struct at_parser *parser, const void *data, size_t len)
{
    const uint8_t *buf = data;

    while (len > 0)
    {
        switch (parser->state)
        {
            case STATE_IDLE:
            case STATE_READLINE:
            case STATE_DATAPROMPT:
            {
                uint8_t ch = *buf++; len--;

                if ((ch != '\r') && (ch != '\n')) {
                    /* Add a newline if there's some preceding content. */
                    if (parser->buf_used > 0 && parser->buf_current == parser->buf_used)
                    {
                        parser_append(parser, '\n');
                        parser->buf_current = parser->buf_used;
                    }

                    /* Append the character if it's not a newline. */
                    parser_append(parser, ch);
                }

                /* Handle full lines. */
                if ((ch == '\n') ||
                    (parser->state == STATE_DATAPROMPT &&
                     parser->buf_used == 2 &&
                     !memcmp(parser->buf, "> ", 2)))
                {
                    parser_handle_line(parser);
                }
            }
            break;

            case STATE_RAWDATA: {
                uint8_t ch = *buf++;

                if (parser->data_left > 0) {
                    parser_append(parser, ch);
                    parser->data_left--;
                }

                if (parser->data_left == 0) {
                    parser_append(parser, '\n');
                    parser->buf_current = parser->buf_used;
                    parser->state = STATE_READLINE;
                }
            } break;

            case STATE_HEXDATA: {
#if 0
                uint8_t ch = *buf++;

                if (parser->data_left > 0) {
                    // TODO
                }

                if (parser->data_left == 0) {
                    parser_append(parser, '\n');
                    parser->state = STATE_READLINE;
                }
#endif
            } break;
        }
    }
}

void at_parser_free(struct at_parser *parser)
{
    free(parser->buf);
    free(parser);
}

/* vim: set ts=4 sw=4 et: */