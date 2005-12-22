/*
 * Copyright (C) 1996-8 Michael R. Elkins <me@mutt.org>
 * Copyright (C) 1996-9 Brandon Long <blong@fiction.net>
 * Copyright (C) 1999-2005 Brendan Cully <brendan@kublai.com>
 * 
 *     This program is free software; you can redistribute it and/or modify
 *     it under the terms of the GNU General Public License as published by
 *     the Free Software Foundation; either version 2 of the License, or
 *     (at your option) any later version.
 * 
 *     This program is distributed in the hope that it will be useful,
 *     but WITHOUT ANY WARRANTY; without even the implied warranty of
 *     MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *     GNU General Public License for more details.
 * 
 *     You should have received a copy of the GNU General Public License
 *     along with this program; if not, write to the Free Software
 *     Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */ 

/* command.c: routines for sending commands to an IMAP server and parsing
 *  responses */

#if HAVE_CONFIG_H
# include "config.h"
#endif

#include "mutt.h"
#include "imap_private.h"
#include "message.h"
#include "mx.h"
#include "buffy.h"

#include <ctype.h>
#include <stdlib.h>

#define IMAP_CMD_BUFSIZE 512

/* forward declarations */
static IMAP_COMMAND* cmd_new (IMAP_DATA* idata);
static int cmd_status (const char *s);
static void cmd_handle_fatal (IMAP_DATA* idata);
static int cmd_handle_untagged (IMAP_DATA* idata);
static void cmd_parse_capabilities (IMAP_DATA* idata, char* s);
static void cmd_parse_expunge (IMAP_DATA* idata, const char* s);
static void cmd_parse_list (IMAP_DATA* idata, char* s);
static void cmd_parse_lsub (IMAP_DATA* idata, char* s);
static void cmd_parse_fetch (IMAP_DATA* idata, char* s);
static void cmd_parse_myrights (IMAP_DATA* idata, const char* s);
static void cmd_parse_search (IMAP_DATA* idata, const char* s);
static void cmd_parse_status (IMAP_DATA* idata, char* s);

static char *Capabilities[] = {
  "IMAP4",
  "IMAP4rev1",
  "STATUS",
  "ACL", 
  "NAMESPACE",
  "AUTH=CRAM-MD5",
  "AUTH=GSSAPI",
  "AUTH=ANONYMOUS",
  "STARTTLS",
  "LOGINDISABLED",
  "IDLE",
  "SASL-IR",

  NULL
};

/* imap_cmd_queue: Add command to command queue. Fails if the queue is full. */
int imap_cmd_queue (IMAP_DATA* idata, const char* cmdstr)
{
  IMAP_COMMAND* cmd;

  if (idata->status == IMAP_FATAL)
  {
    cmd_handle_fatal (idata);
    return IMAP_CMD_BAD;
  }

  if (!(cmd = cmd_new (idata)))
    return IMAP_CMD_BAD;

  mutt_buffer_printf (idata->cmdbuf, "%s%s %s\r\n",
    idata->state == IMAP_IDLE ? "DONE\r\n" : "", cmd->seq, cmdstr);

  if (idata->state == IMAP_IDLE)
    idata->state = IMAP_SELECTED;

  return 0;
}

/* imap_cmd_start: Given an IMAP command, send it to the server.
 *   If cmdstr is NULL, sends queued commands. */
int imap_cmd_start (IMAP_DATA* idata, const char* cmdstr)
{
  int rc;

  if (cmdstr && (rc = imap_cmd_queue (idata, cmdstr)) < 0)
    return rc;

  /* don't write old or empty commands */
  if (idata->cmdbuf->dptr == idata->cmdbuf->data)
    return IMAP_CMD_BAD;

  rc = mutt_socket_write (idata->conn, idata->cmdbuf->data);
  idata->cmdbuf->dptr = idata->cmdbuf->data;

  return (rc < 0) ? IMAP_CMD_BAD : 0;
}

/* imap_cmd_step: Reads server responses from an IMAP command, detects
 *   tagged completion response, handles untagged messages, can read
 *   arbitrarily large strings (using malloc, so don't make it _too_
 *   large!). */
int imap_cmd_step (IMAP_DATA* idata)
{
  size_t len = 0;
  int c;
  int rc;

  if (idata->status == IMAP_FATAL)
  {
    cmd_handle_fatal (idata);
    return IMAP_CMD_BAD;
  }

  /* read into buffer, expanding buffer as necessary until we have a full
   * line */
  do
  {
    if (len == idata->blen)
    {
      safe_realloc (&idata->buf, idata->blen + IMAP_CMD_BUFSIZE);
      idata->blen = idata->blen + IMAP_CMD_BUFSIZE;
      dprint (3, (debugfile, "imap_cmd_step: grew buffer to %u bytes\n",
		  idata->blen));
    }

    /* back up over '\0' */
    if (len)
      len--;
    c = mutt_socket_readln (idata->buf + len, idata->blen - len, idata->conn);
    if (c <= 0)
    {
      dprint (1, (debugfile, "imap_cmd_step: Error reading server response.\n"));
      cmd_handle_fatal (idata);
      return IMAP_CMD_BAD;
    }

    len += c;
  }
  /* if we've read all the way to the end of the buffer, we haven't read a
   * full line (mutt_socket_readln strips the \r, so we always have at least
   * one character free when we've read a full line) */
  while (len == idata->blen);

  /* don't let one large string make cmd->buf hog memory forever */
  if ((idata->blen > IMAP_CMD_BUFSIZE) && (len <= IMAP_CMD_BUFSIZE))
  {
    safe_realloc (&idata->buf, IMAP_CMD_BUFSIZE);
    idata->blen = IMAP_CMD_BUFSIZE;
    dprint (3, (debugfile, "imap_cmd_step: shrank buffer to %u bytes\n", idata->blen));
  }

  idata->lastread = time (NULL);

  /* handle untagged messages. The caller still gets its shot afterwards. */
  if (!ascii_strncmp (idata->buf, "* ", 2) &&
      cmd_handle_untagged (idata))
    return IMAP_CMD_BAD;

  /* server demands a continuation response from us */
  if (idata->buf[0] == '+')
    return IMAP_CMD_RESPOND;

  /* tagged completion code. TODO: I believe commands will always be completed
   * in order, but will have to double-check when I have net again */
  rc = IMAP_CMD_CONTINUE;
  if (!ascii_strncmp (idata->buf, idata->cmds[idata->lastcmd].seq, SEQLEN))
  {
    idata->cmds[idata->lastcmd].state = cmd_status (idata->buf);
    idata->lastcmd = (idata->lastcmd + 1) % IMAP_PIPELINE_DEPTH;
    if (idata->lastcmd == idata->nextcmd)
    {
      rc = cmd_status (idata->buf);
      imap_cmd_finish (idata);
    }
  }

  return rc;
}

/* imap_code: returns 1 if the command result was OK, or 0 if NO or BAD */
int imap_code (const char *s)
{
  return cmd_status (s) == IMAP_CMD_OK;
}

/* imap_exec: execute a command, and wait for the response from the server.
 * Also, handle untagged responses.
 * Flags:
 *   IMAP_CMD_FAIL_OK: the calling procedure can handle failure. This is used
 *     for checking for a mailbox on append and login
 *   IMAP_CMD_PASS: command contains a password. Suppress logging.
 * Return 0 on success, -1 on Failure, -2 on OK Failure
 */
int imap_exec (IMAP_DATA* idata, const char* cmdstr, int flags)
{
  int rc;

  if (idata->status == IMAP_FATAL)
  {
    cmd_handle_fatal (idata);
    return -1;
  }

  if (cmdstr && (rc = imap_cmd_queue (idata, cmdstr)) < 0)
    return rc;
  
  /* don't write old or empty commands */
  if (idata->cmdbuf->dptr == idata->cmdbuf->data)
    return IMAP_CMD_BAD;
  
  rc = mutt_socket_write_d (idata->conn, idata->cmdbuf->data,
    flags & IMAP_CMD_PASS ? IMAP_LOG_PASS : IMAP_LOG_CMD);
  idata->cmdbuf->dptr = idata->cmdbuf->data;

  if (rc < 0)
  {
    cmd_handle_fatal (idata);
    return -1;
  }

  do
    rc = imap_cmd_step (idata);
  while (rc == IMAP_CMD_CONTINUE);

  if (rc == IMAP_CMD_NO && (flags & IMAP_CMD_FAIL_OK))
    return -2;

  if (rc != IMAP_CMD_OK)
  {
    if (flags & IMAP_CMD_FAIL_OK)
      return -2;

    dprint (1, (debugfile, "imap_exec: command failed: %s\n", idata->buf));
    return -1;
  }

  return 0;
}

/* imap_cmd_finish: Attempts to perform cleanup (eg fetch new mail if
 *   detected, do expunge). Called automatically by imap_cmd_step, but
 *   may be called at any time. Called by imap_check_mailbox just before
 *   the index is refreshed, for instance. */
void imap_cmd_finish (IMAP_DATA* idata)
{
  if (idata->status == IMAP_FATAL)
  {
    cmd_handle_fatal (idata);
    return;
  }

  if (!(idata->state >= IMAP_SELECTED) || idata->ctx->closing)
    return;
  
  if (idata->reopen & IMAP_REOPEN_ALLOW)
  {
    int count = idata->newMailCount;

    if (!(idata->reopen & IMAP_EXPUNGE_PENDING) &&
	(idata->reopen & IMAP_NEWMAIL_PENDING)
	&& count > idata->ctx->msgcount)
    {
      /* read new mail messages */
      dprint (2, (debugfile, "imap_cmd_finish: Fetching new mail\n"));
      /* check_status: curs_main uses imap_check_mailbox to detect
       *   whether the index needs updating */
      idata->check_status = IMAP_NEWMAIL_PENDING;
      imap_read_headers (idata, idata->ctx->msgcount, count-1);
    }
    else if (idata->reopen & IMAP_EXPUNGE_PENDING)
    {
      dprint (2, (debugfile, "imap_cmd_finish: Expunging mailbox\n"));
      imap_expunge_mailbox (idata);
      /* Detect whether we've gotten unexpected EXPUNGE messages */
      if (idata->reopen & IMAP_EXPUNGE_PENDING &&
	  !(idata->reopen & IMAP_EXPUNGE_EXPECTED))
	idata->check_status = IMAP_EXPUNGE_PENDING;
      idata->reopen &= ~(IMAP_EXPUNGE_PENDING | IMAP_NEWMAIL_PENDING |
			 IMAP_EXPUNGE_EXPECTED);
    }
  }

  idata->status = 0;
}

/* sets up a new command control block and adds it to the queue.
 * Returns NULL if the pipeline is full. */
static IMAP_COMMAND* cmd_new (IMAP_DATA* idata)
{
  IMAP_COMMAND* cmd;

  if ((idata->nextcmd + 1) % IMAP_PIPELINE_DEPTH == idata->lastcmd)
  {
    dprint (2, (debugfile, "cmd_new: IMAP command queue full\n"));
    return NULL;
  }

  cmd = idata->cmds + idata->nextcmd;
  idata->nextcmd = (idata->nextcmd + 1) % IMAP_PIPELINE_DEPTH;

  snprintf (cmd->seq, sizeof (cmd->seq), "a%04u", idata->seqno++);
  if (idata->seqno > 9999)
    idata->seqno = 0;

  cmd->state = IMAP_CMD_NEW;

  return cmd;
}

/* parse response line for tagged OK/NO/BAD */
static int cmd_status (const char *s)
{
  s = imap_next_word((char*)s);
  
  if (!ascii_strncasecmp("OK", s, 2))
    return IMAP_CMD_OK;
  if (!ascii_strncasecmp("NO", s, 2))
    return IMAP_CMD_NO;

  return IMAP_CMD_BAD;
}

/* cmd_handle_fatal: when IMAP_DATA is in fatal state, do what we can */
static void cmd_handle_fatal (IMAP_DATA* idata)
{
  idata->status = IMAP_FATAL;

  if ((idata->state >= IMAP_SELECTED) &&
      (idata->reopen & IMAP_REOPEN_ALLOW))
  {
    mx_fastclose_mailbox (idata->ctx);
    mutt_error (_("Mailbox closed"));
    mutt_sleep (1);
    idata->state = IMAP_DISCONNECTED;
  }

  if (idata->state < IMAP_SELECTED)
  {
    idata->state = IMAP_DISCONNECTED;
    mutt_socket_close (idata->conn);
    idata->status = 0;
  }
}

/* cmd_handle_untagged: fallback parser for otherwise unhandled messages. */
static int cmd_handle_untagged (IMAP_DATA* idata)
{
  char* s;
  char* pn;
  int count;

  s = imap_next_word (idata->buf);

  if ((idata->state >= IMAP_SELECTED) && isdigit ((unsigned char) *s))
  {
    pn = s;
    s = imap_next_word (s);

    /* EXISTS and EXPUNGE are always related to the SELECTED mailbox for the
     * connection, so update that one.
     */
    if (ascii_strncasecmp ("EXISTS", s, 6) == 0)
    {
      dprint (2, (debugfile, "Handling EXISTS\n"));

      /* new mail arrived */
      count = atoi (pn);

      if ( !(idata->reopen & IMAP_EXPUNGE_PENDING) &&
	   count < idata->ctx->msgcount)
      {
	/* something is wrong because the server reported fewer messages
	 * than we previously saw
	 */
	mutt_error _("Fatal error.  Message count is out of sync!");
	idata->status = IMAP_FATAL;
	return -1;
      }
      /* at least the InterChange server sends EXISTS messages freely,
       * even when there is no new mail */
      else if (count == idata->ctx->msgcount)
	dprint (3, (debugfile,
          "cmd_handle_untagged: superfluous EXISTS message.\n"));
      else
      {
	if (!(idata->reopen & IMAP_EXPUNGE_PENDING))
        {
          dprint (2, (debugfile,
            "cmd_handle_untagged: New mail in %s - %d messages total.\n",
            idata->mailbox, count));
	  idata->reopen |= IMAP_NEWMAIL_PENDING;
        }
	idata->newMailCount = count;
      }
    }
    /* pn vs. s: need initial seqno */
    else if (ascii_strncasecmp ("EXPUNGE", s, 7) == 0)
      cmd_parse_expunge (idata, pn);
    else if (ascii_strncasecmp ("FETCH", s, 5) == 0)
      cmd_parse_fetch (idata, pn);
  }
  else if (ascii_strncasecmp ("CAPABILITY", s, 10) == 0)
    cmd_parse_capabilities (idata, s);
  else if (ascii_strncasecmp ("LIST", s, 4) == 0)
    cmd_parse_list (idata, s);
  else if (ascii_strncasecmp ("LSUB", s, 4) == 0)
    cmd_parse_lsub (idata, s);
  else if (ascii_strncasecmp ("MYRIGHTS", s, 8) == 0)
    cmd_parse_myrights (idata, s);
  else if (ascii_strncasecmp ("SEARCH", s, 6) == 0)
    cmd_parse_search (idata, s);
  else if (ascii_strncasecmp ("STATUS", s, 6) == 0)
    cmd_parse_status (idata, s);
  else if (ascii_strncasecmp ("BYE", s, 3) == 0)
  {
    dprint (2, (debugfile, "Handling BYE\n"));

    /* check if we're logging out */
    if (idata->status == IMAP_BYE)
      return 0;

    /* server shut down our connection */
    s += 3;
    SKIPWS (s);
    mutt_error ("%s", s);
    mutt_sleep (2);
    cmd_handle_fatal (idata);

    return -1;
  }
  else if (option (OPTIMAPSERVERNOISE) && (ascii_strncasecmp ("NO", s, 2) == 0))
  {
    dprint (2, (debugfile, "Handling untagged NO\n"));

    /* Display the warning message from the server */
    mutt_error ("%s", s+3);
    mutt_sleep (2);
  }

  return 0;
}

/* cmd_parse_capabilities: set capability bits according to CAPABILITY
 *   response */
static void cmd_parse_capabilities (IMAP_DATA* idata, char* s)
{
  int x;

  dprint (2, (debugfile, "Handling CAPABILITY\n"));

  s = imap_next_word (s);
  FREE(&idata->capstr);
  idata->capstr = safe_strdup (s);

  memset (idata->capabilities, 0, sizeof (idata->capabilities));

  while (*s)
  {
    for (x = 0; x < CAPMAX; x++)
      if (imap_wordcasecmp(Capabilities[x], s) == 0)
      {
	mutt_bit_set (idata->capabilities, x);
	break;
      }
    s = imap_next_word (s);
  }
}

/* cmd_parse_expunge: mark headers with new sequence ID and mark idata to
 *   be reopened at our earliest convenience */
static void cmd_parse_expunge (IMAP_DATA* idata, const char* s)
{
  int expno, cur;
  HEADER* h;

  dprint (2, (debugfile, "Handling EXPUNGE\n"));

  expno = atoi (s);

  /* walk headers, zero seqno of expunged message, decrement seqno of those
   * above. Possibly we could avoid walking the whole list by resorting
   * and guessing a good starting point, but I'm guessing the resort would
   * nullify the gains */
  for (cur = 0; cur < idata->ctx->msgcount; cur++)
  {
    h = idata->ctx->hdrs[cur];

    if (h->index+1 == expno)
      h->index = -1;
    else if (h->index+1 > expno)
      h->index--;
  }

  idata->reopen |= IMAP_EXPUNGE_PENDING;
}

/* cmd_parse_fetch: Load fetch response into IMAP_DATA. Currently only
 *   handles unanticipated FETCH responses, and only FLAGS data. We get
 *   these if another client has changed flags for a mailbox we've selected.
 *   Of course, a lot of code here duplicates code in message.c. */
static void cmd_parse_fetch (IMAP_DATA* idata, char* s)
{
  int msgno, cur;
  HEADER* h = NULL;

  dprint (2, (debugfile, "Handling FETCH\n"));

  msgno = atoi (s);
  
  if (msgno <= idata->ctx->msgcount)
  /* see cmd_parse_expunge */
    for (cur = 0; cur < idata->ctx->msgcount; cur++)
    {
      h = idata->ctx->hdrs[cur];
      
      if (h->active && h->index+1 == msgno)
      {
	dprint (2, (debugfile, "Message UID %d updated\n", HEADER_DATA(h)->uid));
	break;
      }
      
      h = NULL;
    }
  
  if (!h)
  {
    dprint (1, (debugfile, "FETCH response ignored for this message\n"));
    return;
  }
  
  /* skip FETCH */
  s = imap_next_word (s);
  s = imap_next_word (s);

  if (*s != '(')
  {
    dprint (1, (debugfile, "Malformed FETCH response"));
    return;
  }
  s++;

  if (ascii_strncasecmp ("FLAGS", s, 5) != 0)
  {
    dprint (2, (debugfile, "Only handle FLAGS updates\n"));
    return;
  }

  /* If server flags could conflict with mutt's flags, reopen the mailbox. */
  if (h->changed)
    idata->reopen |= IMAP_EXPUNGE_PENDING;
  else {
    imap_set_flags (idata, h, s);
    idata->check_status = IMAP_FLAGS_PENDING;
  }
}

static void cmd_parse_list (IMAP_DATA* idata, char* s)
{
  IMAP_LIST* list;
  IMAP_LIST lb;
  char delimbuf[5]; /* worst case: "\\"\0 */

  if (idata->cmddata)
    list = (IMAP_LIST*)idata->cmddata;
  else
    list = &lb;

  memset (list, 0, sizeof (IMAP_LIST));

  /* flags */
  s = imap_next_word (s);
  if (*s != '(')
  {
    dprint (1, (debugfile, "Bad LIST response\n"));
    return;
  }
  s++;
  while (*s)
  {
    if (!ascii_strncasecmp (s, "\\NoSelect", 9))
      list->noselect = 1;
    else if (!ascii_strncasecmp (s, "\\NoInferiors", 12))
      list->noinferiors = 1;
    /* See draft-gahrns-imap-child-mailbox-?? */
    else if (!ascii_strncasecmp (s, "\\HasNoChildren", 14))
      list->noinferiors = 1;
    
    s = imap_next_word (s);
    if (*(s - 2) == ')')
      break;
  }

  /* Delimiter */
  if (ascii_strncasecmp (s, "NIL", 3))
  {
    delimbuf[0] = '\0';
    safe_strcat (delimbuf, 5, s); 
    imap_unquote_string (delimbuf);
    list->delim = delimbuf[0];
  }

  /* Name */
  s = imap_next_word (s);
  imap_unmunge_mbox_name (s);
  list->name = s;
  
  if (list->name[0] == '\0')
  {
    idata->delim = list->delim;
    dprint (2, (debugfile, "Root delimiter: %c\n", idata->delim));
  }
}

static void cmd_parse_lsub (IMAP_DATA* idata, char* s)
{
  char buf[STRING];
  char errstr[STRING];
  BUFFER err, token;
  ciss_url_t url;
  IMAP_LIST list;

  if (idata->cmddata)
  {
    /* caller will handle response itself */
    cmd_parse_list (idata, s);
    return;
  }

  if (!option (OPTIMAPCHECKSUBSCRIBED))
    return;

  idata->cmddata = &list;
  cmd_parse_list (idata, s);
  idata->cmddata = NULL;
  if (!list.name)
    return;

  dprint (2, (debugfile, "Subscribing to %s\n", list.name));

  strfcpy (buf, "mailboxes \"", sizeof (buf));
  mutt_account_tourl (&idata->conn->account, &url);
  url.path = list.name;
  if (!mutt_strcmp (url.user, ImapUser))
    url.user = NULL;
  url_ciss_tostring (&url, buf + 11, sizeof (buf) - 10, 0);
  safe_strcat (buf, sizeof (buf), "\"");
  memset (&token, 0, sizeof (token));
  err.data = errstr;
  err.dsize = sizeof (errstr);
  if (mutt_parse_rc_line (buf, &token, &err))
    dprint (1, (debugfile, "Error adding subscribed mailbox: %s\n", errstr));
  FREE (&token.data);
}

/* cmd_parse_myrights: set rights bits according to MYRIGHTS response */
static void cmd_parse_myrights (IMAP_DATA* idata, const char* s)
{
  dprint (2, (debugfile, "Handling MYRIGHTS\n"));

  s = imap_next_word ((char*)s);
  s = imap_next_word ((char*)s);

  /* zero out current rights set */
  memset (idata->rights, 0, sizeof (idata->rights));

  while (*s && !isspace((unsigned char) *s))
  {
    switch (*s) 
    {
      case 'l':
	mutt_bit_set (idata->rights, IMAP_ACL_LOOKUP);
	break;
      case 'r':
	mutt_bit_set (idata->rights, IMAP_ACL_READ);
	break;
      case 's':
	mutt_bit_set (idata->rights, IMAP_ACL_SEEN);
	break;
      case 'w':
	mutt_bit_set (idata->rights, IMAP_ACL_WRITE);
	break;
      case 'i':
	mutt_bit_set (idata->rights, IMAP_ACL_INSERT);
	break;
      case 'p':
	mutt_bit_set (idata->rights, IMAP_ACL_POST);
	break;
      case 'c':
	mutt_bit_set (idata->rights, IMAP_ACL_CREATE);
	break;
      case 'd':
	mutt_bit_set (idata->rights, IMAP_ACL_DELETE);
	break;
      case 'a':
	mutt_bit_set (idata->rights, IMAP_ACL_ADMIN);
	break;
    }
    s++;
  }
}

/* This should be optimised (eg with a tree or hash) */
static int uid2msgno (IMAP_DATA* idata, unsigned int uid)
{
  int i;
  
  for (i = 0; i < idata->ctx->msgcount; i++)
  {
    HEADER* h = idata->ctx->hdrs[i];
    if (HEADER_DATA(h)->uid == uid)
      return i;
  }
  
  return -1;
}

/* cmd_parse_search: store SEARCH response for later use */
static void cmd_parse_search (IMAP_DATA* idata, const char* s)
{
  unsigned int uid;
  int msgno;

  dprint (2, (debugfile, "Handling SEARCH\n"));

  while ((s = imap_next_word ((char*)s)) && *s != '\0')
  {
    uid = atoi (s);
    msgno = uid2msgno (idata, uid);
    
    if (msgno >= 0)
      idata->ctx->hdrs[uid2msgno (idata, uid)]->matched = 1;
  }
}

/* first cut: just do buffy update. Later we may wish to cache all
 * mailbox information, even that not desired by buffy */
static void cmd_parse_status (IMAP_DATA* idata, char* s)
{
  char* mailbox;
  char* value;
  BUFFY* inc;
  IMAP_MBOX mx;
  int count;
  IMAP_STATUS *status, sb;
  int olduv, oldun;

  mailbox = imap_next_word (s);
  s = imap_next_word (mailbox);
  *(s - 1) = '\0';
  imap_unmunge_mbox_name (mailbox);

  if (!(status = imap_mboxcache_get (idata, mailbox)))
  {
    /* ugly interface - why should I look up what I just added? */
    memset (&sb, 0, sizeof (IMAP_STATUS));
    sb.name = mailbox;
    idata->mboxcache = mutt_add_list_n (idata->mboxcache, &sb, sizeof (IMAP_STATUS));
    status = imap_mboxcache_get (idata, mailbox);
    status->name = safe_strdup (mailbox);
  }
  olduv = status->uidvalidity;
  oldun = status->uidnext;

  if (*s++ != '(')
  {
    dprint (1, (debugfile, "Error parsing STATUS\n"));
    return;
  }
  while (*s && *s != ')')
  {
    value = imap_next_word (s);
    count = strtol (value, &value, 10);

    if (!ascii_strncmp ("MESSAGES", s, 8))
      status->messages = count;
    else if (!ascii_strncmp ("RECENT", s, 6))
      status->recent = count;
    else if (!ascii_strncmp ("UIDNEXT", s, 7))
      status->uidnext = count;
    else if (!ascii_strncmp ("UIDVALIDITY", s, 11))
      status->uidvalidity = count;
    else if (!ascii_strncmp ("UNSEEN", s, 6))
      status->unseen = count;

    s = value;
    if (*s && *s != ')')
      s = imap_next_word (s);
  }
  dprint (2, (debugfile, "%s (UIDVALIDITY: %d, UIDNEXT: %d) %d messages, %d recent, %d unseen\n",
              status->name, status->uidvalidity, status->uidnext,
              status->messages, status->recent, status->unseen));

  /* caller is prepared to handle the result herself */
  if (idata->cmddata)
  {
    memcpy (idata->cmddata, status, sizeof (IMAP_STATUS));
    return;
  }

  dprint (2, (debugfile, "Running default STATUS handler\n"));

  /* should perhaps move this code back to imap_buffy_check */
  for (inc = Incoming; inc; inc = inc->next)
  {
    if (inc->magic != M_IMAP)
      continue;
    
    if (imap_parse_path (inc->path, &mx) < 0)
    {
      dprint (1, (debugfile, "Error parsing mailbox %s, skipping\n", inc->path));
      continue;
    }
    /* dprint (2, (debugfile, "Buffy entry: [%s] mbox: [%s]\n", inc->path, NONULL(mx.mbox))); */
    
    if (mutt_account_match (&idata->conn->account, &mx.account))
    {
      value = safe_strdup (mx.mbox);
      imap_fix_path (idata, mx.mbox, value, mutt_strlen (value) + 1);
      FREE (&mx.mbox);

      if (value && !imap_mxcmp (mailbox, value))
      {
        dprint (2, (debugfile, "Found %s in buffy list (OV: %d ON: %d U: %d)\n",
                    mailbox, olduv, oldun, status->unseen));
        
        if (olduv && olduv == status->uidvalidity)
        {
          if (oldun < status->uidnext)
          {
            inc->new = status->unseen;
          }
        }
        else
          inc->new = status->unseen;

        /* forced back to keep detecting new mail until the mailbox is opened */
        status->uidnext = oldun;

        FREE (&value);
        return;
      }
    }

    FREE (&mx.mbox);
  }
}
