/*
 * Copyright (c) 2013 Todd Mortimer
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <unicode/utf.h>
#include <unicode/ucnv.h>
#include <unicode/ustring.h>
#include <clipboard/clipboard.h>


#include "terminal.h"
#include "preferences.h"

#include "io.h"

/* from ecma48.c */
extern int ecma48_bracketed_paste();

/* from main.c: byte trace of what the terminal sends to the shell */
extern void io_trace_write(const char *buf, size_t len, ssize_t wrote);

static int master_fd = -1;
/* One converter per direction. ICU converters are stateful: the read side
 * holds back the first bytes of a multi-byte character until the rest
 * arrives. Sharing one converter between reading shell output and encoding
 * typed input (and the keyhold upcase lookup) let that held-back state leak
 * into unrelated conversions, which garbled echoed CJK text. */
static UConverter* tty_in_conv;     /* shell output  -> UChar (stateful) */
static UConverter* tty_out_conv;    /* typed input   -> tty encoding */
static UConverter* tty_peek_conv;   /* last write    -> UChar (upcase) */

static UConverter* utf8_conv;
static UErrorCode  utf8_conv_err = U_ZERO_ERROR;

static char readbuf[READ_BUFFER_SIZE];
static char writebuf[CHARACTER_BUFFER * U8_MAX_LENGTH];
static char* writebufLimit;

int io_init(pref_t *prefs){

	// create converters
	UErrorCode e1 = U_ZERO_ERROR, e2 = U_ZERO_ERROR, e3 = U_ZERO_ERROR;
	tty_in_conv   = ucnv_open(prefs->tty_encoding, &e1);
	tty_out_conv  = ucnv_open(prefs->tty_encoding, &e2);
	tty_peek_conv = ucnv_open(prefs->tty_encoding, &e3);
	utf8_conv  = ucnv_open("UTF-8", &utf8_conv_err);
	if(U_FAILURE(e1) || U_FAILURE(e2) || U_FAILURE(e3)){
		fprintf(stderr, "Could not open converters for %s\n", prefs->tty_encoding);
	}
	return TERM_SUCCESS;
}

void io_uninit(){

	// free converts
	if(tty_in_conv != NULL){ ucnv_close(tty_in_conv); tty_in_conv = NULL; }
	if(tty_out_conv != NULL){ ucnv_close(tty_out_conv); tty_out_conv = NULL; }
	if(tty_peek_conv != NULL){ ucnv_close(tty_peek_conv); tty_peek_conv = NULL; }
	if(utf8_conv != NULL){ ucnv_close(utf8_conv); utf8_conv = NULL; }
  /* the session owning this fd closed it already; closing the number
   * again could hit an unrelated file opened in between */
  if(master_fd >= 0){ close(master_fd); }
  master_fd = -1;
}

void io_set_master(int fd){
	master_fd = fd;
}

int io_get_master(){
	return master_fd;
}

/* The last thing we wrote is still in writebuf.
 * This function will set buf to the character _after_ the
 * last output character, which lets the caller find the
 * upcase length. The return value is the length of the
 * last written string, which tells the caller how many
 * backspaces to send in order to erase it.
 * A return value of 0 indicates no upcase representation,
 * or an error occurred. */
int32_t io_upcase_last_write(UChar **buf, int32_t nUChar){
	UChar c[CHARACTER_BUFFER];
	UChar *target;
  UChar *targetLimit;
	const char *source;
	const char *sourceLimit;
  int32_t uCaseLen;
  int32_t lCaseLen;
  UErrorCode uCaseErr = U_ZERO_ERROR;

	/* Convert from output encoding to UChar first, then upcase */
	target = c;
	targetLimit = c + CHARACTER_BUFFER;
	source = writebuf;
	sourceLimit = writebufLimit;
	{
		UErrorCode err = U_ZERO_ERROR;
		ucnv_reset(tty_peek_conv);
		ucnv_toUnicode(tty_peek_conv, &target, targetLimit, &source, sourceLimit, NULL, TRUE, &err);
		if(err == U_BUFFER_OVERFLOW_ERROR){
			fprintf(stderr, "ucnv_toUnicode() in io_upcase_last_write ran out of target buffer\n");
		}
	}
	lCaseLen = (int32_t)(target - c);
	uCaseLen = u_strToUpper(*buf, nUChar, c, lCaseLen, NULL, &uCaseErr);
	if (uCaseLen > nUChar){
		// output was truncated
		uCaseLen = nUChar;
	}
	/* If we got something */ //, and it isn't the same as before */
	if ((uCaseLen > 0)     ){ // && (u_strCompare(*buf, uCaseLen, c, lCaseLen, TRUE) != 0)){
		// point _after_ the last UChar
		*buf = *buf + uCaseLen;
		return lCaseLen;
	}
	// no upcase value / error / no dice..
	return 0;
}

/* The master pty is O_NONBLOCK, so a write can come up short - or fail
 * outright with EAGAIN - whenever the child is not draining its end fast
 * enough. Ignoring that loses the tail of a key sequence (an arrow key
 * arriving as a bare ESC) or most of a paste, which reads as a dead
 * keyboard. Retry until the buffer is gone, with a bound so a child that
 * has stopped reading for good cannot wedge the caller: this runs on the
 * UI thread with the input lock held. */
#define WRITE_RETRY_USEC  1000
#define WRITE_RETRY_LIMIT 200   /* ~200ms */
static ssize_t write_all_master_raw(const char* buf, size_t len);
static ssize_t write_all_master(const char* buf, size_t len){
  ssize_t w = write_all_master_raw(buf, len);
  io_trace_write(buf, len, w);
  return w;
}

static ssize_t write_all_master_raw(const char* buf, size_t len){
  size_t off = 0;
  int tries = 0;
  while(off < len){
    ssize_t w = write(master_fd, buf + off, len - off);
    if(w > 0){
      off += (size_t)w;
      tries = 0;
      continue;
    }
    if(w < 0 && (errno == EAGAIN || errno == EINTR)){
      if(++tries > WRITE_RETRY_LIMIT){
        fprintf(stderr, "Gave up writing %d of %d bytes to the pty\n",
                (int)(len - off), (int)len);
        break;
      }
      usleep(WRITE_RETRY_USEC);
      continue;
    }
    /* a real error */
    if(off == 0){
      return w;
    }
    break;
  }
  return (ssize_t)off;
}

ssize_t io_write_master(const UChar *buf, size_t nUChar){

	char* target = writebuf;
	char* targetLimit = writebuf + (CHARACTER_BUFFER * U8_MAX_LENGTH);
	const UChar* source = buf;
	const UChar* sourceLimit = buf + nUChar;

	{
		UErrorCode err = U_ZERO_ERROR;
		ucnv_fromUnicode(tty_out_conv, &target, targetLimit, &source, sourceLimit, NULL, TRUE, &err);
		if(err == U_BUFFER_OVERFLOW_ERROR){
			fprintf(stderr, "ucnv_fromUnicode() in io_write_master ran out of target buffer\n");
		} else if(U_FAILURE(err)){
			/* never let a sticky error swallow every later key */
			ucnv_reset(tty_out_conv);
		}
	}

  writebufLimit = target;
	return write_all_master(writebuf, (size_t)(target - writebuf));
}

ssize_t io_write_master_char(const char *buf, size_t n){
  return write_all_master(buf, n);
}

ssize_t io_read_master(UChar *buf, size_t nUChar){
	const char *source;
	const char *sourceLimit;
	int32_t count;
  UChar *target;
  UChar *targetLimit;

  /* Read nUChar bytes, which necessarily translates into <= nUChar UChars */
	count = read(master_fd, readbuf, nUChar);
	if(count <= 0){
		return count;
	}
	// else

	source = readbuf;
	sourceLimit = readbuf + count;

	target = buf;
	targetLimit = buf + nUChar;

  {
    UErrorCode err = U_ZERO_ERROR;
    ucnv_toUnicode(tty_in_conv, &target, targetLimit, &source, sourceLimit, NULL, FALSE, &err);
    if(err == U_BUFFER_OVERFLOW_ERROR){
      fprintf(stderr, "ucnv_toUnicode() in io_read_master ran out of target buffer\n");
    } else if(U_FAILURE(err)){
      ucnv_reset(tty_in_conv);
    }
  }

  /* Bytes arrived but they are only the start of a multi-byte character
   * (e.g. 1-2 bytes of a 3-byte UTF-8 CJK glyph): the converter keeps them
   * for the next read and produced nothing yet. That is "no data yet", not
   * end of file - returning 0 here made the caller treat the pty as closed
   * and kill the session whenever a CJK character was split across reads. */
  if(target == buf){
    errno = EAGAIN;
    return -1;
  }
  return (ssize_t)(target - buf);
}

ssize_t io_read_utf8_string(const char* utf8, size_t utf8len, UChar* buf){

	const char *source;
	const char *sourceLimit;
  UChar *target;
  UChar *targetLimit;


	source = utf8;
	sourceLimit = utf8 + utf8len;

	target = buf;
	targetLimit = buf + utf8len;

  ucnv_toUnicode(utf8_conv, &target, targetLimit, &source, sourceLimit, NULL, TRUE, &utf8_conv_err);

  if(utf8_conv_err == U_BUFFER_OVERFLOW_ERROR){
  	fprintf(stderr, "ucnv_toUnicode() in io_read_utf8_string ran out of target buffer when converting from utf8\n");
  	utf8_conv_err = U_ZERO_ERROR;
  }

  return (ssize_t)(target - buf);
}

void io_paste_from_clipboard(){
  char* buffer = NULL;
  int ret;
  if(is_clipboard_format_present("text/plain") == 0){
    ret = get_clipboard_data("text/plain", &buffer);
    if(ret > 0 && buffer != NULL){
    	/* Paste and Pray. The encoding of the byte stream will be whatever
    	 * the copied source was - hopefully tty_encoding..
    	 */
      if(ecma48_bracketed_paste()){
        /* xterm bracketed paste: let the application distinguish pasted
         * text from typed text */
        write_all_master("\033[200~", 6);
        write_all_master(buffer, ret * sizeof(char));
        write_all_master("\033[201~", 6);
      } else {
        write_all_master(buffer, ret * sizeof(char));
      }
      free(buffer);
    }
  }
}

/* copy UTF-16 text to the system clipboard as UTF-8 */
void io_copy_to_clipboard(const UChar* text, int len){
  UErrorCode err = U_ZERO_ERROR;
  int32_t u8len = 0;
  char* u8 = NULL;

  u_strToUTF8(NULL, 0, &u8len, text, len, &err);
  if(u8len <= 0){
    return;
  }
  u8 = malloc(u8len + 1);
  if(u8 == NULL){
    return;
  }
  err = U_ZERO_ERROR;
  u_strToUTF8(u8, u8len + 1, NULL, text, len, &err);
  if(!U_FAILURE(err)){
    empty_clipboard();
    set_clipboard_data("text/plain", u8len, u8);
  }
  free(u8);
}

/* set the system clipboard to raw (UTF-8) bytes, e.g. from OSC 52 */
void io_set_clipboard_bytes(const char* data, int len){
  if(data != NULL && len > 0){
    empty_clipboard();
    set_clipboard_data("text/plain", len, (char*)data);
  }
}
