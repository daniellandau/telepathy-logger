/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */
/*
 * Copyright (C) 2009 Collabora Ltd.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * Authors: Cosimo Alfarano <cosimo.alfarano@collabora.co.uk>
 */

#include "config.h"
#include "channel-text.h"

#include <glib.h>
#include <telepathy-glib/contact.h>
#include <telepathy-glib/enums.h>
#include <telepathy-glib/proxy.h>

#include <telepathy-logger/action-chain.h>
#include <telepathy-logger/contact.h>
#include <telepathy-logger/channel.h>
#include <telepathy-logger/observer.h>
#include <telepathy-logger/log-entry-text.h>
#include <telepathy-logger/log-manager-priv.h>
#include <telepathy-logger/log-store-sqlite.h>
#include <telepathy-logger/datetime.h>
#include <telepathy-logger/util.h>


#define DEBUG_FLAG TPL_DEBUG_CHANNEL
#include <telepathy-logger/debug.h>

#define TP_CONTACT_MYSELF 0
#define TP_CONTACT_REMOTE 1

#define GET_PRIV(obj)    TPL_GET_PRIV (obj, TplChannelText)
struct _TplChannelTextPriv
{
  gboolean chatroom;
  TpContact *my_contact;
  TpContact *remote_contact;  /* only set if chatroom==FALSE */
  gchar *chatroom_id;          /* only set if chatroom==TRUE */

  /* only used as metadata in CB data passing */
  guint selector;
};

static TpContactFeature features[3] = {
  TP_CONTACT_FEATURE_ALIAS,
  TP_CONTACT_FEATURE_PRESENCE,
  TP_CONTACT_FEATURE_AVATAR_TOKEN
};

static void call_when_ready_wrapper (TplChannel *tpl_chan,
    GAsyncReadyCallback cb, gpointer user_data);
void got_pending_messages_cb (TpChannel *proxy, const GPtrArray *result,
    const GError *error, gpointer user_data, GObject *weak_object);

static void got_tpl_chan_ready_cb (GObject *obj, GAsyncResult *result,
    gpointer user_data);
static void on_closed_cb (TpChannel *proxy, gpointer user_data,
    GObject *weak_object);
static void on_lost_message_cb (TpChannel *proxy, gpointer user_data,
    GObject *weak_object);
static void on_received_signal_cb (TpChannel *proxy, guint arg_ID,
    guint arg_Timestamp, guint arg_Sender, guint arg_Type, guint arg_Flags,
    const gchar *arg_Text, gpointer user_data, GObject *weak_object);
static void on_sent_signal_cb (TpChannel *proxy, guint arg_Timestamp,
    guint arg_Type, const gchar *arg_Text,  gpointer user_data,
    GObject *weak_object);
static void on_send_error_cb (TpChannel *proxy, guint arg_Error,
    guint arg_Timestamp, guint arg_Type, const gchar *arg_Text,
    gpointer user_data, GObject *weak_object);
static void on_pending_messages_removed_cb (TpChannel *proxy,
    const GArray *arg_Message_IDs, gpointer user_data, GObject *weak_object);

static void pendingproc_connect_signals (TplActionChain *ctx,
    gpointer user_data);
static void pendingproc_get_pending_messages (TplActionChain *ctx,
   gpointer user_data);
static void pendingproc_prepare_tpl_channel (TplActionChain *ctx,
   gpointer user_data);
static void pendingproc_get_chatroom_id (TplActionChain *ctx,
   gpointer user_data);
static void get_chatroom_id_cb (TpConnection *proxy,
    const gchar **identifiers, const GError *error, gpointer user_data,
    GObject *weak_object);
static void pendingproc_get_my_contact (TplActionChain *ctx,
   gpointer user_data);
static void pendingproc_get_remote_contact (TplActionChain *ctx,
   gpointer user_data);
static void pendingproc_get_remote_handle_type (TplActionChain *ctx,
   gpointer user_data);
static void pendingproc_cleanup_pending_messages_db (TplActionChain *ctx,
    gpointer user_data);

static void keepon_on_receiving_signal (TplLogEntryText *log);
static void got_message_pending_messages_cb (TpProxy *proxy,
    const GValue *out_Value, const GError *error, gpointer user_data,
    GObject *weak_object);
static void got_text_pending_messages_cb (TpChannel *proxy,
    const GPtrArray *result, const GError *error, gpointer user_data,
    GObject *weak_object);

G_DEFINE_TYPE (TplChannelText, tpl_channel_text, TPL_TYPE_CHANNEL)

/* used by _get_my_contact and _get_remote_contact */
static void
got_contact_cb (TpConnection *connection,
    guint n_contacts,
    TpContact *const *contacts,
    guint n_failed,
    const TpHandle *failed,
    const GError *error,
    gpointer user_data,
    GObject *weak_object)
{
  TplObserver *observer = tpl_observer_new (); /* singleton */
  TplActionChain *ctx = user_data;
  TplChannelText *tpl_text = tpl_actionchain_get_object (ctx);
  TplChannelTextPriv *priv = GET_PRIV (tpl_text);
  TplChannel *tpl_chan = TPL_CHANNEL (tpl_text);
  TpChannel *tp_chan = TP_CHANNEL (tpl_chan);

  g_return_if_fail (TPL_IS_CHANNEL_TEXT (tpl_text));

  g_assert_cmpuint (n_failed, ==, 0);
  g_assert_cmpuint (n_contacts, ==, 1);
  g_assert_cmpuint (priv->selector, <=, TP_CONTACT_REMOTE);

  if (n_failed > 0)
    {
      TpConnection *tp_conn = tp_channel_borrow_connection (tp_chan);
      const gchar *conn_path;

      conn_path = tp_proxy_get_object_path (TP_PROXY (tp_conn));

      PATH_DEBUG (tpl_text, "Error resolving self handle for connection %s."
         " Aborting channel observation", conn_path);
      tpl_observer_unregister_channel (observer, TPL_CHANNEL (tpl_text));

      g_object_unref (observer);
      tpl_actionchain_terminate (ctx);
      return;
    }

  switch (priv->selector)
    {
      case TP_CONTACT_MYSELF:
        tpl_channel_text_set_my_contact (tpl_text, contacts[0]);
        break;
      case TP_CONTACT_REMOTE:
        tpl_channel_text_set_remote_contact (tpl_text, contacts[0]);
        break;
      default:
        PATH_DEBUG (tpl_text, "retrieving TpContacts: passing invalid value"
            " for selector: %d Aborting channel observation", priv->selector);
        tpl_observer_unregister_channel (observer, TPL_CHANNEL (tpl_text));
        g_object_unref (observer);
        tpl_actionchain_terminate (ctx);
        return;
    }

  g_object_unref (observer);
  tpl_actionchain_continue (ctx);
}


static void
pendingproc_get_remote_contact (TplActionChain *ctx,
    gpointer user_data)
{
  TplChannelText *tpl_text = tpl_actionchain_get_object (ctx);
  TplChannel *tpl_chan = TPL_CHANNEL (tpl_text);
  TpHandle remote_handle;
  TpConnection *tp_conn = tp_channel_borrow_connection (TP_CHANNEL (
        tpl_chan));

  remote_handle = tp_channel_get_handle (TP_CHANNEL (tpl_chan), NULL);

  GET_PRIV (tpl_text)->selector = TP_CONTACT_REMOTE;
  tp_connection_get_contacts_by_handle (tp_conn, 1, &remote_handle,
      G_N_ELEMENTS (features), features, got_contact_cb, ctx, NULL, NULL);
}


static void
pendingproc_get_my_contact (TplActionChain *ctx,
    gpointer user_data)
{
  TplChannelText *tpl_text = tpl_actionchain_get_object (ctx);
  TpConnection *tp_conn = tp_channel_borrow_connection (
      TP_CHANNEL (tpl_text));
  TpHandle my_handle = tp_connection_get_self_handle (tp_conn);

  GET_PRIV (tpl_text)->selector = TP_CONTACT_MYSELF;
  tp_connection_get_contacts_by_handle (tp_conn, 1, &my_handle,
      G_N_ELEMENTS (features), features, got_contact_cb, ctx, NULL, NULL);
}


static void
pendingproc_get_remote_handle_type (TplActionChain *ctx,
    gpointer user_data)
{
  TplChannelText *tpl_text = tpl_actionchain_get_object (ctx);
  TpHandleType remote_handle_type;

  tp_channel_get_handle (TP_CHANNEL (tpl_text), &remote_handle_type);

  switch (remote_handle_type)
    {
      case TP_HANDLE_TYPE_CONTACT:
        tpl_actionchain_prepend (ctx, pendingproc_get_remote_contact, NULL);
        break;
      case TP_HANDLE_TYPE_ROOM:
        tpl_actionchain_prepend (ctx, pendingproc_get_chatroom_id, NULL);
        break;
      case TP_HANDLE_TYPE_NONE:
        PATH_DEBUG (tpl_text, "HANDLE_TYPE_NONE received, probably an anonymous "
            "chat, like MSN ones. TODO: implement this possibility");
        tpl_actionchain_terminate (ctx);
        return;
        break;
      /* follows unhandled TpHandleType */
      case TP_HANDLE_TYPE_LIST:
        PATH_DEBUG (tpl_text, "remote handle: TP_HANDLE_TYPE_LIST: "
            "un-handled. Check the TelepathyLogger.client file.");
        tpl_actionchain_terminate (ctx);
        return;
        break;
      case TP_HANDLE_TYPE_GROUP:
        PATH_DEBUG (tpl_text, "remote handle: TP_HANDLE_TYPE_GROUP: "
            "un-handled. Check the TelepathyLogger.client file.");
        tpl_actionchain_terminate (ctx);
        return;
        break;
      default:
        PATH_DEBUG (tpl_text, "remote handle type unknown %d.",
            remote_handle_type);
        tpl_actionchain_terminate (ctx);
        return;
        break;
    }

  tpl_actionchain_continue (ctx);
}
/* end of async Callbacks */


static void
tpl_channel_text_dispose (GObject *obj)
{
  TplChannelTextPriv *priv = GET_PRIV (obj);

  if (priv->my_contact != NULL)
    {
      g_object_unref (priv->my_contact);
      priv->my_contact = NULL;
    }
  if (priv->remote_contact != NULL)
    {
      g_object_unref (priv->remote_contact);
      priv->remote_contact = NULL;
    }

  G_OBJECT_CLASS (tpl_channel_text_parent_class)->dispose (obj);
}


static void
tpl_channel_text_finalize (GObject *obj)
{
  TplChannelTextPriv *priv = GET_PRIV (obj);

  g_free (priv->chatroom_id);
  priv->chatroom_id = NULL;

  G_OBJECT_CLASS (tpl_channel_text_parent_class)->finalize (obj);
}


static void
tpl_channel_text_class_init (TplChannelTextClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  TplChannelClass *tpl_chan_class = TPL_CHANNEL_CLASS (klass);

  object_class->dispose = tpl_channel_text_dispose;
  object_class->finalize = tpl_channel_text_finalize;

  tpl_chan_class->call_when_ready = call_when_ready_wrapper;

  g_type_class_add_private (object_class, sizeof (TplChannelTextPriv));
}


static void
tpl_channel_text_init (TplChannelText *self)
{
  TplChannelTextPriv *priv = G_TYPE_INSTANCE_GET_PRIVATE (self,
      TPL_TYPE_CHANNEL_TEXT, TplChannelTextPriv);

  self->priv = priv;
}


/**
 * tpl_channel_text_new
 * @conn: TpConnection instance owning the channel
 * @object_path: the channel's DBus path
 * @tp_chan_props: channel's immutable properties, obtained for example by
 * %tp_channel_borrow_immutable_properties()
 * @account: TpAccount instance, related to the new #TplChannelText
 * @error: location of the GError, used in case a problem is raised while
 * creating the channel
 *
 * Convenience function to create a new TPL Channel Text proxy.
 * The returned #TplChannelText is not guaranteed to be ready at the point of
 * return.
 *
 * TplChannelText is actually a subclass of the abstract TplChannel which is a
 * subclass of TpChannel.
 * Use #TpChannel methods, casting the #TplChannelText instance to a
 * TpChannel, to access TpChannel data/methods from it.
 *
 * TplChannelText is usually created using #tpl_channel_factory_build, from
 * within a #TplObserver singleton, when its Observer_Channel method is called
 * by the Channel Dispatcher.
 *
 * Returns: the TplChannelText instance or %NULL in @object_path is not valid
 */
TplChannelText *
tpl_channel_text_new (TpConnection *conn,
    const gchar *object_path,
    GHashTable *tp_chan_props,
    TpAccount *account,
    GError **error)
{
  TpProxy *conn_proxy = TP_PROXY (conn);

  /* Do what tpl_channel_new does + set TplChannelText specific properties */

  g_return_val_if_fail (TP_IS_CONNECTION (conn), NULL);
  g_return_val_if_fail (TP_IS_ACCOUNT (account), NULL);
  g_return_val_if_fail (!TPL_STR_EMPTY (object_path), NULL);
  g_return_val_if_fail (tp_chan_props != NULL, NULL);

  if (!tp_dbus_check_valid_object_path (object_path, error))
    return NULL;

  return g_object_new (TPL_TYPE_CHANNEL_TEXT,
      /* TplChannel properties */
      "account", account,
      /* TpChannel properties */
      "connection", conn,
      "dbus-daemon", conn_proxy->dbus_daemon,
      "bus-name", conn_proxy->bus_name,
      "object-path", object_path,
      "handle-type", (guint) TP_UNKNOWN_HANDLE_TYPE,
      "channel-properties", tp_chan_props,
      NULL);
}


TpContact *
tpl_channel_text_get_remote_contact (TplChannelText *self)
{
  TplChannelTextPriv *priv = GET_PRIV (self);

  g_return_val_if_fail (TPL_IS_CHANNEL_TEXT (self), NULL);

  return priv->remote_contact;
}


TpContact *
tpl_channel_text_get_my_contact (TplChannelText *self)
{
  TplChannelTextPriv *priv = GET_PRIV (self);

  g_return_val_if_fail (TPL_IS_CHANNEL_TEXT (self), NULL);

  return priv->my_contact;
}


gboolean
tpl_channel_text_is_chatroom (TplChannelText *self)
{
  TplChannelTextPriv *priv = GET_PRIV (self);

  g_return_val_if_fail (TPL_IS_CHANNEL_TEXT (self), FALSE);

  return priv->chatroom;
}


const gchar *
tpl_channel_text_get_chatroom_id (TplChannelText *self)
{
  TplChannelTextPriv *priv = GET_PRIV (self);

  g_return_val_if_fail (TPL_IS_CHANNEL_TEXT (self), NULL);

  return priv->chatroom_id;
}


void
tpl_channel_text_set_remote_contact (TplChannelText *self,
    TpContact *data)
{
  TplChannelTextPriv *priv = GET_PRIV (self);

  g_return_if_fail (TPL_IS_CHANNEL_TEXT (self));
  g_return_if_fail (TP_IS_CONTACT (data));
  g_return_if_fail (priv->remote_contact == NULL);

  priv->remote_contact = g_object_ref (data);
}


void
tpl_channel_text_set_my_contact (TplChannelText *self,
    TpContact *data)
{
  TplChannelTextPriv *priv = GET_PRIV (self);

  g_return_if_fail (TPL_IS_CHANNEL_TEXT (self));
  g_return_if_fail (TP_IS_CONTACT (data));
  g_return_if_fail (priv->my_contact == NULL);

  priv->my_contact = g_object_ref (data);
}


void
tpl_channel_text_set_chatroom (TplChannelText *self,
    gboolean data)
{
  TplChannelTextPriv *priv = GET_PRIV (self);

  g_return_if_fail (TPL_IS_CHANNEL_TEXT (self));

  priv->chatroom = data;
}


void
tpl_channel_text_set_chatroom_id (TplChannelText *self,
    const gchar *data)
{
  TplChannelTextPriv *priv = GET_PRIV (self);

  g_return_if_fail (TPL_IS_CHANNEL_TEXT (self));
  g_return_if_fail (!TPL_STR_EMPTY (data));
  g_return_if_fail (priv->chatroom_id == NULL);
  priv->chatroom_id = g_strdup (data);
}


static void
call_when_ready_wrapper (TplChannel *tpl_chan,
    GAsyncReadyCallback cb,
    gpointer user_data)
{
  tpl_channel_text_call_when_ready (TPL_CHANNEL_TEXT (tpl_chan), cb,
      user_data);
}


void
tpl_channel_text_call_when_ready (TplChannelText *self,
    GAsyncReadyCallback cb, gpointer user_data)
{
  TplActionChain *actions;

  /* first: connect signals, so none are lost
   * second: prepare all TplChannel
   * third: cache my contact and the remote one.
   * last: check for pending messages
   *
   * If for any reason, the order is changed, it's needed to check what objects
   * are unreferenced by g_object_unref but used by a next action AND what object are actually not
   * prepared but used anyway */
  actions = tpl_actionchain_new (G_OBJECT (self), cb, user_data);
  tpl_actionchain_append (actions, pendingproc_connect_signals, NULL);
  tpl_actionchain_append (actions, pendingproc_prepare_tpl_channel, NULL);
  tpl_actionchain_append (actions, pendingproc_get_my_contact, NULL);
  tpl_actionchain_append (actions, pendingproc_get_remote_handle_type, NULL);
  tpl_actionchain_append (actions, pendingproc_get_pending_messages, NULL);
  tpl_actionchain_append (actions, pendingproc_cleanup_pending_messages_db, NULL);
  /* start the chain consuming */
  tpl_actionchain_continue (actions);
}


static void
pendingproc_prepare_tpl_channel (TplActionChain *ctx,
    gpointer user_data)
{
  TplChannel *tpl_chan = TPL_CHANNEL (tpl_actionchain_get_object (ctx));

  TPL_CHANNEL_GET_CLASS (tpl_chan)->call_when_ready_protected (tpl_chan,
      got_tpl_chan_ready_cb, ctx);
}


static void
got_tpl_chan_ready_cb (GObject *obj,
    GAsyncResult *tpl_chan_result,
    gpointer user_data)
{
  TplActionChain *ctx = user_data;

  /* if TplChannel preparation is OK, keep on with the TplChannelText */
  if (tpl_actionchain_finish (tpl_chan_result))
    tpl_actionchain_continue (ctx);
  else
     tpl_actionchain_terminate (ctx);
  return;
}


/* Cleans up stale log-ids in the index logstore.
 *
 * It 'brutally' considers as stale all log-ids which timestamp is older than
 * <time_limit> days AND are still not set as aknowledged.
 *
 * NOTE: While retrieving open channels, a partial clean-up for the channel's
 * stale pending messages is done. It's not enough, since it doesn't consider
 * all the channel that was closed at retrieval time.  This functions try to
 * catch stale ids in the rest of the DB, heuristically.
 *
 * It is wrong to consider all the log-ids not having an channel currently
 * open as stale, since a channel might be temporarely disconnected and
 * reconnected and some protocols might repropose not acknowledged messages on
 * reconnection. We need to consider only reasonably old log-ids.
 *
 * This function is meant only to reduce the size of the DB used for indexing.
 *
 * No tpl_actionchain_terminate() is called if some fatal error occurs since
 * it's not considered a crucial point for TplChannel preparation.
 */
static void
pendingproc_cleanup_pending_messages_db (TplActionChain *ctx,
    gpointer user_data)
{
  /* five days ago in seconds */
  const time_t time_limit = tpl_time_get_current () -
    TPL_LOG_STORE_SQLITE_CLEANUP_DELTA_LIMIT;
  TplLogStore *index = tpl_log_store_sqlite_dup ();
  GList *l;
  GError *error = NULL;

  if (index == NULL)
    {
      DEBUG ("Unable to obtain the TplLogStoreIndex singleton");
      goto out;
    }

  l = tpl_log_store_sqlite_get_log_ids (index, NULL, time_limit,
      &error);
  if (error != NULL)
    {
      DEBUG ("unable to obtain log-id in Index DB: %s", error->message);
      g_error_free (error);
      /* do not call tpl_actionchain_terminate, if it's temporary next startup
       * TPL will re-do the clean-up. If it's fatal, the flow will stop later
       * anyway */
      goto out;
    }

  while (l != NULL)
    {
      gchar *log_id = l->data;

      /* brutally ACK the stale message and ignore any error */
      tpl_log_store_sqlite_set_acknowledgment (index, log_id, NULL);

      g_free (log_id);
      l = g_list_remove_link (l, l);
    }

out:
  if (index != NULL)
    g_object_unref (index);

  tpl_actionchain_continue (ctx);
}

static void
pendingproc_get_pending_messages (TplActionChain *ctx,
    gpointer user_data)
{
  TplChannelText *chan_text = tpl_actionchain_get_object (ctx);

  if (tp_proxy_has_interface (chan_text,
        "org.freedesktop.Telepathy.Channel.Interface.Messages"))
    tp_cli_dbus_properties_call_get (chan_text, -1,
        "org.freedesktop.Telepathy.Channel.Interface.Messages",
        "PendingMessages", got_message_pending_messages_cb, ctx, NULL, NULL);
  else
    tp_cli_channel_type_text_call_list_pending_messages (TP_CHANNEL (chan_text),
        -1, FALSE, got_text_pending_messages_cb, ctx, NULL, NULL);
}

/* PendingMessages CB for Message interface */
static void
got_message_pending_messages_cb (TpProxy *proxy,
    const GValue *out_Value,
    const GError *error,
    gpointer user_data,
    GObject *weak_object)
{
  const gchar *channel_path = tp_proxy_get_object_path (proxy);
  TplLogStore *index = tpl_log_store_sqlite_dup ();
  TplActionChain *ctx = user_data;
  GPtrArray *result = NULL;
  GList *indexed_pending_msg = NULL;
  GError *loc_error = NULL;
  guint i;

  if (!TPL_IS_CHANNEL (proxy))
    goto out;

  if (error != NULL)
    {
      CRITICAL ("retrieving messages for Message iface: %s", error->message);
      goto out;
    }

  /* It's aaa{vs}, a list of message each containing a list of message's parts
   * each contain a dictioanry k:v */
  result = g_value_get_boxed (out_Value);

  /* getting messages ids known to be pending at last TPL exit */
  indexed_pending_msg = tpl_log_store_sqlite_get_pending_messages (index,
      TP_CHANNEL (proxy), &loc_error);
  if (loc_error != NULL)
    {
      CRITICAL ("Unable to obtain pending messages stored in TPL DB: %s",
          loc_error->message);
      goto out;
    }

  /* cycle the list of messages */
  PATH_DEBUG (proxy, "%d pending message(s) from Message iface", result->len);
  PATH_DEBUG (proxy, "Checking if there are any un-logged messages among "
      "pending messages");
  for (i = 0; i < result->len; ++i)
    {
      GPtrArray *message_parts;
      GHashTable *message_headers; /* string:gvalue */
      GHashTable *message_part; /* string:gvalue */
      const gchar *message_token;
      gchar *tpl_message_token;
      guint64 message_timestamp;
      guint message_type = TP_CHANNEL_TEXT_MESSAGE_TYPE_NORMAL;
      guint message_flags = 0;
      guint message_id;
      TpHandle message_sender_handle;

      gboolean is_scrollback;
      gboolean is_rescued;
      const gchar *message_body;
      GList *l = NULL;

      /* list of message's parts */
      message_parts = g_ptr_array_index (result, i);

      /* message part 0 is the message's headers */
      message_headers = g_ptr_array_index (message_parts, 0);
      /* message part 1 is is the first part, the most 'faithful' among
       * alternatives.
       * TODO fully support alternatives and attachments/images
       * related to them */
      message_part = g_ptr_array_index (message_parts, 1);

      message_token = tp_asv_get_string (message_headers, "message-token");
      message_id = tp_asv_get_uint32 (message_headers, "pending-message-id",
          NULL);
      message_timestamp = tp_asv_get_uint64 (message_headers,
          "message-received", NULL);

      tpl_message_token = create_message_token (channel_path,
          tpl_time_to_string_local (message_timestamp, "%Y%m%d%H%M%S"),
          message_id);

      /* look for the current token among the TPL indexed tokens/log_id */
      l = g_list_find_custom (indexed_pending_msg, tpl_message_token,
            (GCompareFunc) g_strcmp0);
      if (l != NULL)
        {
          PATH_DEBUG (proxy, "pending msg %s already logged, not logging",
              tpl_message_token);
          /* I remove the element so that at the end of the cycle I'll have
           * only elements that I could not find in the pending msg list,
           * which are stale elements */
          indexed_pending_msg = g_list_remove_link (indexed_pending_msg, l);
          g_free (l->data);
          g_list_free (l);

          /* do not log messages which log_id is present in LogStoreIndex */
          continue;
        }

      message_sender_handle = tp_asv_get_uint32 (message_headers,
          "message-sender", NULL);

      if (g_hash_table_lookup (message_headers, "message-type") != NULL)
        message_type = tp_asv_get_uint32 (message_headers, "message-type",
            NULL);

      is_rescued = tp_asv_get_boolean (message_headers, "rescued", NULL);
      is_scrollback = tp_asv_get_boolean (message_headers, "scrollback",
          NULL);
      message_flags = (is_rescued ? TP_CHANNEL_TEXT_MESSAGE_FLAG_RESCUED : 0);
      message_flags |= (is_scrollback ?
          TP_CHANNEL_TEXT_MESSAGE_FLAG_SCROLLBACK : 0);

      message_body = tp_asv_get_string (message_part, "content");

      on_received_signal_cb (TP_CHANNEL (proxy), message_id, message_timestamp,
          message_sender_handle, message_type, message_flags, message_body,
          TPL_CHANNEL_TEXT (proxy),
          NULL);

      g_free (tpl_message_token);
    }

  /* Remove messages not set as ACK but not in the pending queue anymore: they
   * are stale entries which was already ACK while TPL was 'down'.
   *
   * NOTE: this will clean up stale entries in index, related to any channel
   * currently open, we don't know anything about all the other stale entries
   * related to channel not currently open.
   */
  PATH_DEBUG (proxy, "Cleaning up stale messages");
  while (indexed_pending_msg != NULL)
    {
      gchar *log_id = indexed_pending_msg->data;

      PATH_DEBUG (proxy, "%s is stale, removing from DB", log_id);
      tpl_log_store_sqlite_set_acknowledgment (index, log_id, &loc_error);
      if (loc_error != NULL)
        {
          CRITICAL ("Unable to set %s as acknoledged in TPL DB: %s", log_id,
              loc_error->message);
          g_clear_error (&loc_error);
        }
      g_free (log_id);
      /* free list's head, which will return the next element, if any */
      indexed_pending_msg = g_list_delete_link (indexed_pending_msg,
          indexed_pending_msg);
    }
  PATH_DEBUG (proxy, "Clean up finished.");

out:
  if (index != NULL)
    g_object_unref (index);

  if (loc_error != NULL)
      g_error_free (loc_error);

  tpl_actionchain_continue (ctx);
}


/* PendingMessages CB for Text interface */
static void
got_text_pending_messages_cb (TpChannel *proxy,
    const GPtrArray *result,
    const GError *error,
    gpointer user_data,
    GObject *weak_object)
{
  TplActionChain *ctx = user_data;
  guint i;

  if (error != NULL)
    {
      PATH_DEBUG (proxy, "retrieving pending messages for Text iface: %s", error->message);
      tpl_actionchain_terminate (ctx);
      return;
    }

  PATH_DEBUG (proxy, "%d pending message(s) for Text iface", result->len);
  for (i = 0; i < result->len; ++i)
    {
      GValueArray *message_struct;
      const gchar *message_body;
      guint message_id;
      guint message_timestamp;
      guint from_handle;
      guint message_type;
      guint message_flags;

      message_struct = g_ptr_array_index (result, i);

      message_id = g_value_get_uint (g_value_array_get_nth (message_struct, 0));
      message_timestamp = g_value_get_uint (g_value_array_get_nth (
            message_struct, 1));
      from_handle = g_value_get_uint (g_value_array_get_nth (message_struct, 2));
      message_type = g_value_get_uint (g_value_array_get_nth (message_struct, 3));
      message_flags = g_value_get_uint (g_value_array_get_nth (message_struct, 4));
      message_body = g_value_get_string (g_value_array_get_nth (message_struct, 5));


      /* call the received signal callback to trigger the message storing */
      on_received_signal_cb (proxy, message_id, message_timestamp, from_handle,
          message_type, message_flags, message_body, TPL_CHANNEL_TEXT (proxy),
          NULL);
    }

  tpl_actionchain_continue (ctx);
}


static void
pendingproc_get_chatroom_id (TplActionChain *ctx,
    gpointer user_data)
{
  TplChannelText *tpl_text = tpl_actionchain_get_object (ctx);
  TplChannel *tpl_chan = TPL_CHANNEL (tpl_text);
  TpConnection *connection = tp_channel_borrow_connection (TP_CHANNEL (
        tpl_chan));
  TpHandle room_handle;
  GArray handles = { (gchar *) &room_handle, 1 };

  room_handle = tp_channel_get_handle (TP_CHANNEL (tpl_chan), NULL);

  tpl_channel_text_set_chatroom (tpl_text, TRUE);
  tp_cli_connection_call_inspect_handles (connection,
      -1, TP_HANDLE_TYPE_ROOM, &handles, get_chatroom_id_cb,
      ctx, NULL, NULL);
}


static void
get_chatroom_id_cb (TpConnection *proxy,
    const gchar **identifiers,
    const GError *error,
    gpointer user_data,
    GObject *weak_object)
{
  TplActionChain *ctx = user_data;
  TplChannelText *tpl_text = tpl_actionchain_get_object (ctx);

  g_return_if_fail (TPL_IS_CHANNEL_TEXT (tpl_text));

  if (error != NULL)
    {
      PATH_DEBUG (proxy, "retrieving chatroom identifier: %s", error->message);
      tpl_actionchain_terminate (ctx);
      return;
    }

  PATH_DEBUG (proxy, "Chatroom id: %s", identifiers[0]);
  tpl_channel_text_set_chatroom_id (tpl_text, identifiers[0]);

  tpl_actionchain_continue (ctx);
}


static void
pendingproc_connect_signals (TplActionChain *ctx,
    gpointer user_data)
{
  TplChannelText *tpl_text = tpl_actionchain_get_object (ctx);
  GError *error = NULL;
  gboolean is_error = FALSE;
  TpChannel *channel = NULL;

  channel = TP_CHANNEL (TPL_CHANNEL (tpl_text));

  tp_cli_channel_type_text_connect_to_received (channel,
      on_received_signal_cb, tpl_text, NULL, NULL, &error);
  if (error != NULL)
    {
      PATH_DEBUG (tpl_text, "'received' signal connect: %s", error->message);
      g_clear_error (&error);
      is_error = TRUE;
    }

  tp_cli_channel_type_text_connect_to_sent (channel,
      on_sent_signal_cb, tpl_text, NULL, NULL, &error);
  if (error != NULL)
    {
      PATH_DEBUG (tpl_text, "'sent' signal connect: %s", error->message);
      g_clear_error (&error);
      is_error = TRUE;
    }

  tp_cli_channel_type_text_connect_to_send_error (channel,
      on_send_error_cb, tpl_text, NULL, NULL, &error);
  if (error != NULL)
    {
      PATH_DEBUG (tpl_text, "'send error' signal connect: %s", error->message);
      g_clear_error (&error);
      is_error = TRUE;
    }

  tp_cli_channel_type_text_connect_to_lost_message (channel,
      on_lost_message_cb, tpl_text, NULL, NULL, &error);
  if (error != NULL)
    {
      PATH_DEBUG (tpl_text, "'lost message' signal connect: %s", error->message);
      g_clear_error (&error);
      is_error = TRUE;
    }

  tp_cli_channel_connect_to_closed (channel, on_closed_cb,
      tpl_text, NULL, NULL, &error);
  if (error != NULL)
    {
      PATH_DEBUG (tpl_text, "'closed' signal connect: %s", error->message);
      g_clear_error (&error);
      is_error = TRUE;
    }

  tp_cli_channel_interface_messages_connect_to_pending_messages_removed (
      channel, on_pending_messages_removed_cb, NULL, NULL,
      G_OBJECT (tpl_text), &error);
  if (error != NULL)
    {
      PATH_DEBUG (tpl_text, "'PendingMessagesRemoved' signal connect: %s",
          error->message);
      g_clear_error (&error);
      is_error = TRUE;
    }

  /* TODO connect to TpContacts' notify::presence-type */

  if (is_error)
    tpl_actionchain_terminate (ctx);
  else
    tpl_actionchain_continue (ctx);
}



/* Signal's Callbacks */
static void
on_pending_messages_removed_cb (TpChannel *proxy,
    const GArray *arg_Message_IDs,
    gpointer user_data,
    GObject *weak_object)
{
  TplLogStore *index = tpl_log_store_sqlite_dup ();
  guint i;
  GError *error = NULL;

  for (i = 0; i < arg_Message_IDs->len; ++i)
    {
      guint msg_id = g_array_index (arg_Message_IDs, guint, i);
      tpl_log_store_sqlite_set_acknowledgment_by_msg_id (index, proxy, msg_id,
          &error);
      PATH_DEBUG (proxy, "msg_id %d acknowledged", msg_id);
      if (error != NULL)
        {
          PATH_DEBUG (proxy, "cannot set the ACK flag for msg_id %d: %s",
              msg_id, error->message);
          g_clear_error (&error);
        }
    }

  if (index != NULL)
    g_object_unref (index);
}


static void
on_closed_cb (TpChannel *proxy,
    gpointer user_data,
    GObject *weak_object)
{
  TplChannelText *tpl_text = TPL_CHANNEL_TEXT (user_data);
  TplChannel *tpl_chan = TPL_CHANNEL (tpl_text);
  TplObserver *observer = tpl_observer_new ();

  if (!tpl_observer_unregister_channel (observer, tpl_chan))
    PATH_DEBUG (tpl_chan, "Channel couldn't be unregistered correctly (BUG?)");

  g_object_unref (observer);
}


static void
on_lost_message_cb (TpChannel *proxy,
           gpointer user_data,
           GObject *weak_object)
{
  PATH_DEBUG (proxy, "lost message signal catched. nothing logged");
  /* TODO log that the system lost a message */
}


static void
on_send_error_cb (TpChannel *proxy,
         guint arg_Error,
         guint arg_Timestamp,
         guint arg_Type,
         const gchar *arg_Text,
         gpointer user_data,
         GObject *weak_object)
{
  PATH_DEBUG (proxy, "unlogged event: TP was unable to send the message: %s",
      arg_Text);
  /* TODO log that the system was unable to send the message */
}


static void
on_sent_signal_cb (TpChannel *proxy,
    guint arg_Timestamp,
    guint arg_Type,
    const gchar *arg_Text,
    gpointer user_data,
    GObject *weak_object)
{
  GError *error = NULL;
  TplChannelText *tpl_text = TPL_CHANNEL_TEXT (user_data);
  TpContact *remote = NULL;
  TpContact *me;
  TplContact *tpl_contact_sender;
  TplContact *tpl_contact_receiver = NULL;
  TplLogEntryText *log;
  TplLogManager *logmanager;
  const gchar *chat_id;
  const gchar *account_path;
  const gchar *channel_path;
  gchar *log_id;

  g_return_if_fail (TPL_IS_CHANNEL_TEXT (tpl_text));

  channel_path = tp_proxy_get_object_path (TP_PROXY (tpl_text));
  log_id = create_message_token (channel_path,
      tpl_time_to_string_local (arg_Timestamp, "%Y%m%d%H%M%S"), G_MAXUINT);

  /* Initialize data for TplContact */
  me = tpl_channel_text_get_my_contact (tpl_text);
  tpl_contact_sender = tpl_contact_from_tp_contact (me);
  tpl_contact_set_contact_type (tpl_contact_sender, TPL_CONTACT_USER);

  if (!tpl_channel_text_is_chatroom (tpl_text))
    {
      remote = tpl_channel_text_get_remote_contact (tpl_text);
      if (remote == NULL)
        PATH_DEBUG (tpl_text, "sending message: Remote TplContact=NULL on 1-1"
            "Chat");
      tpl_contact_receiver = tpl_contact_from_tp_contact (remote);
      tpl_contact_set_contact_type (tpl_contact_receiver, TPL_CONTACT_USER);

      DEBUG ("sent:\n\tlog_id=\"%s\"\n\tto=\"%s (%s)\"\n\tfrom=\"%s (%s)\"\n\tmsg=\"%s\"",
          log_id,
          tpl_contact_get_identifier (tpl_contact_receiver),
          tpl_contact_get_alias (tpl_contact_receiver),
          tpl_contact_get_identifier (tpl_contact_sender),
          tpl_contact_get_alias (tpl_contact_sender),
          arg_Text);

    }
  else
    {
      DEBUG ("sent:\n\tlog_id=\"%s\"\n\tto chatroom=\"%s\"\n\tfrom=\"%s (%s)\"\n\tmsg=\"%s\"",
          log_id,
          tpl_channel_text_get_chatroom_id (tpl_text),
          tpl_contact_get_identifier (tpl_contact_sender),
          tpl_contact_get_alias (tpl_contact_sender),
          arg_Text);
    }

  /* Initialise TplLogEntryText */
  if (!tpl_channel_text_is_chatroom (tpl_text))
    chat_id = tpl_contact_get_identifier (tpl_contact_receiver);
  else
    chat_id = tpl_channel_text_get_chatroom_id (tpl_text);

  account_path = tp_proxy_get_object_path (
      TP_PROXY (tpl_channel_get_account (TPL_CHANNEL (tpl_text))));

  log = tpl_log_entry_text_new (log_id, account_path,
      TPL_LOG_ENTRY_DIRECTION_OUT);

  tpl_log_entry_set_pending_msg_id (TPL_LOG_ENTRY (log),
      TPL_LOG_ENTRY_MSG_ID_ACKNOWLEDGED);
  tpl_log_entry_set_channel_path (TPL_LOG_ENTRY (log), channel_path);
  tpl_log_entry_text_set_chat_id (log, chat_id);
  tpl_log_entry_text_set_timestamp (log, (time_t) arg_Timestamp);
  tpl_log_entry_text_set_signal_type (log, TPL_LOG_ENTRY_TEXT_SIGNAL_SENT);
  tpl_log_entry_text_set_sender (log, tpl_contact_sender);
  /* NULL when it's a chatroom */
  if (tpl_contact_receiver != NULL)
    tpl_log_entry_text_set_receiver (log, tpl_contact_receiver);
  tpl_log_entry_text_set_message (log, arg_Text);
  tpl_log_entry_text_set_message_type (log, arg_Type);
  tpl_log_entry_text_set_tpl_channel_text (log, tpl_text);

  /* Initialized LogStore and send the log entry */
  tpl_log_entry_text_set_chatroom (log,
      tpl_channel_text_is_chatroom (tpl_text));

  logmanager = tpl_log_manager_dup_singleton ();
  tpl_log_manager_add_message (logmanager, TPL_LOG_ENTRY (log), &error);

  if (error != NULL)
    {
      PATH_DEBUG (tpl_text, "LogStore: %s", error->message);
      g_error_free (error);
    }

  if (tpl_contact_receiver != NULL)
    g_object_unref (tpl_contact_receiver);
  g_object_unref (tpl_contact_sender);
  g_object_unref (logmanager);
  g_object_unref (log);

  g_free (log_id);
}


static void
on_received_signal_with_contact_cb (TpConnection *connection,
    guint n_contacts,
    TpContact *const *contacts,
    guint n_failed,
    const TpHandle *failed,
    const GError *error,
    gpointer user_data,
    GObject *weak_object)
{
  TplLogEntryText *log = TPL_LOG_ENTRY_TEXT (user_data);
  TplChannelText *tpl_text;
  TpContact *remote;

  g_return_if_fail (TPL_IS_LOG_ENTRY_TEXT (log));

  tpl_text = tpl_log_entry_text_get_tpl_channel_text (log);

  if (error != NULL)
    {
      PATH_DEBUG (tpl_text, "An Unrecoverable error retrieving remote contact "
         "information occured: %s", error->message);
      PATH_DEBUG (tpl_text, "Unable to log the received message: %s",
         tpl_log_entry_text_get_message (log));
      g_object_unref (log);
      return;
    }

  if (n_failed > 0)
    {
      PATH_DEBUG (tpl_text, "%d invalid handle(s) passed to "
         "tp_connection_get_contacts_by_handle()", n_failed);
      PATH_DEBUG (tpl_text, "Not able to log the received message: %s",
         tpl_log_entry_text_get_message (log));
      g_object_unref (log);
      return;
    }

  remote = contacts[0];
  tpl_channel_text_set_remote_contact (tpl_text, remote);

  keepon_on_receiving_signal (log);
}


static void
keepon_on_receiving_signal (TplLogEntryText *log)
{
  TplChannelText *tpl_text;
  GError *e = NULL;
  TplLogManager *logmanager;
  TplContact *tpl_contact_sender;
  TplContact *tpl_contact_receiver;
  TpContact *remote;
  TpContact *local;

  g_return_if_fail (TPL_IS_LOG_ENTRY_TEXT (log));

  tpl_text = tpl_log_entry_text_get_tpl_channel_text (log);
  remote = tpl_channel_text_get_remote_contact (tpl_text);
  local = tpl_channel_text_get_my_contact (tpl_text);

  tpl_contact_sender = tpl_contact_from_tp_contact (remote);
  tpl_contact_set_contact_type (tpl_contact_sender, TPL_CONTACT_USER);
  tpl_log_entry_text_set_sender (log, tpl_contact_sender);

  tpl_contact_receiver = tpl_contact_from_tp_contact (local);

  DEBUG ("recvd:\n\tlog_id=\"%s\"\n\tto=\"%s (%s)\"\n\tfrom=\"%s (%s)\"\n\tmsg=\"%s\"",
      tpl_log_entry_get_log_id (TPL_LOG_ENTRY (log)),
      tpl_contact_get_identifier (tpl_contact_receiver),
      tpl_contact_get_alias (tpl_contact_receiver),
      tpl_contact_get_identifier (tpl_contact_sender),
      tpl_contact_get_alias (tpl_contact_sender),
      tpl_log_entry_text_get_message (log));

  /* Initialise LogStore and store the message */

  if (!tpl_channel_text_is_chatroom (tpl_text))
    tpl_log_entry_text_set_chat_id (log, tpl_contact_get_identifier (
          tpl_contact_sender));
  else
    tpl_log_entry_text_set_chat_id (log, tpl_channel_text_get_chatroom_id (
          tpl_text));

  tpl_log_entry_text_set_chatroom (log,
      tpl_channel_text_is_chatroom (tpl_text));

  logmanager = tpl_log_manager_dup_singleton ();
  tpl_log_manager_add_message (logmanager, TPL_LOG_ENTRY (log), &e);
  if (e != NULL)
    {
      DEBUG ("LogStore: %s", e->message);
      g_error_free (e);
    }

  g_object_unref (tpl_contact_sender);
  g_object_unref (logmanager);
}


static void
on_received_signal_cb (TpChannel *proxy,
    guint arg_ID,
    guint arg_Timestamp,
    guint arg_Sender,
    guint arg_Type,
    guint arg_Flags,
    const gchar *arg_Text,
    gpointer user_data,
    GObject *weak_object)
{
  TpHandle remote_handle = (TpHandle) arg_Sender;
  TplChannelText *tpl_text = TPL_CHANNEL_TEXT (user_data);
  TplChannel *tpl_chan = TPL_CHANNEL (tpl_text);
  TpConnection *tp_conn;
  TpContact *me;
  TplContact *tpl_contact_receiver = NULL;
  TplLogEntryText *log;
  TpAccount *account = tpl_channel_get_account (TPL_CHANNEL (tpl_text));
  TplLogStore *index = tpl_log_store_sqlite_dup ();
  const gchar *account_path = tp_proxy_get_object_path (TP_PROXY (account));
  const gchar *channel_path = tp_proxy_get_object_path (TP_PROXY (tpl_text));
  gchar *log_id = create_message_token (channel_path,
      tpl_time_to_string_local (arg_Timestamp, "%Y%m%d%H%M%S"), arg_ID);

  /* First, check if log_id has already been logged
   *
   * FIXME: There is a race condition for which, right after a 'NewChannel'
   * signal is raised and a message is received, the 'received' signal handler
   * may be cateched before or being slower and arriving after the TplChannel
   * preparation (in which pending message list is examined)
   *
   * Workaround:
   * In the first case the analisys of P.M.L will detect that actually the
   * handler has already received and logged the message.
   * In the latter (here), the handler will detect that the P.M.L analisys
   * has found and logged it, returning immediatly */
  if (tpl_log_store_sqlite_log_id_is_present (index, log_id))
    {
      PATH_DEBUG (tpl_text, "%s found, not logging", log_id);
      goto out;
    }

  /* TODO use the Message iface to check the delivery
     notification and handle it correctly */
  if (arg_Flags & TP_CHANNEL_TEXT_MESSAGE_FLAG_NON_TEXT_CONTENT)
    {
      PATH_DEBUG (tpl_text, "Non text content flag set. "
          "Probably a delivery notification for a sent message. "
          "Ignoring");
      return;
    }

  /* Initialize TplLogEntryText (part 1) - chat_id still unknown */
  log = tpl_log_entry_text_new (log_id, account_path,
      TPL_LOG_ENTRY_DIRECTION_IN);

  tpl_log_entry_set_channel_path (TPL_LOG_ENTRY (log), channel_path);
  tpl_log_entry_set_pending_msg_id (TPL_LOG_ENTRY (log), arg_ID);
  tpl_log_entry_text_set_tpl_channel_text (log, tpl_text);
  tpl_log_entry_text_set_message (log, arg_Text);
  tpl_log_entry_text_set_message_type (log, arg_Type);
  tpl_log_entry_text_set_signal_type (log, TPL_LOG_ENTRY_TEXT_SIGNAL_RECEIVED);

  me = tpl_channel_text_get_my_contact (tpl_text);
  tpl_contact_receiver = tpl_contact_from_tp_contact (me);
  tpl_contact_set_contact_type (tpl_contact_receiver, TPL_CONTACT_USER);
  tpl_log_entry_text_set_receiver (log, tpl_contact_receiver);

  tpl_log_entry_text_set_timestamp (log, (time_t) arg_Timestamp);

  tp_conn = tp_channel_borrow_connection (TP_CHANNEL (tpl_chan));
  /* it's a chatroom and no contact has been pre-cached */
  if (tpl_channel_text_get_remote_contact (tpl_text) == NULL)
    tp_connection_get_contacts_by_handle (tp_conn, 1, &remote_handle,
        G_N_ELEMENTS (features), features, on_received_signal_with_contact_cb,
        log, g_object_unref, NULL);
  else
    keepon_on_receiving_signal (log);

out:
  if (tpl_contact_receiver != NULL)
    g_object_unref (tpl_contact_receiver);
  g_object_unref (index);
  g_free (log_id);
}
/* End of Signal's Callbacks */

