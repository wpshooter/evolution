/*
 * e-mail-parser.c
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "evolution-config.h"

#include "e-mail-parser.h"

#include <string.h>

#include <libebackend/libebackend.h>

#include <shell/e-shell.h>
#include <shell/e-shell-window.h>

#include "e-mail-parser-extension.h"
#include "e-mail-part-attachment.h"
#include "e-mail-part-utils.h"

#define E_MAIL_PARSER_GET_PRIVATE(obj) \
	(G_TYPE_INSTANCE_GET_PRIVATE \
	((obj), E_TYPE_MAIL_PARSER, EMailParserPrivate))

#define d(x)

struct _EMailParserPrivate {
	GMutex mutex;

	gint last_error;

	CamelSession *session;
	GHashTable *ongoing_part_lists; /* GCancellable * ~> EMailPartList * */
};

enum {
	PROP_0,
	PROP_SESSION
};

/* internal parser extensions */
GType e_mail_parser_application_mbox_get_type (void);
GType e_mail_parser_audio_get_type (void);
GType e_mail_parser_headers_get_type (void);
GType e_mail_parser_message_get_type (void);
GType e_mail_parser_secure_button_get_type (void);
GType e_mail_parser_source_get_type (void);
GType e_mail_parser_image_get_type (void);
GType e_mail_parser_inline_pgp_encrypted_get_type (void);
GType e_mail_parser_inline_pgp_signed_get_type (void);
GType e_mail_parser_message_delivery_status_get_type (void);
GType e_mail_parser_message_external_get_type (void);
GType e_mail_parser_message_rfc822_get_type (void);
GType e_mail_parser_multipart_alternative_get_type (void);
GType e_mail_parser_multipart_apple_double_get_type (void);
GType e_mail_parser_multipart_digest_get_type (void);
GType e_mail_parser_multipart_encrypted_get_type (void);
GType e_mail_parser_multipart_mixed_get_type (void);
GType e_mail_parser_multipart_related_get_type (void);
GType e_mail_parser_multipart_signed_get_type (void);
GType e_mail_parser_text_enriched_get_type (void);
GType e_mail_parser_text_html_get_type (void);
GType e_mail_parser_text_plain_get_type (void);
#ifdef ENABLE_SMIME
GType e_mail_parser_application_smime_get_type (void);
#endif

static gpointer parent_class;

static void
mail_parser_move_security_before_headers (GQueue *part_queue)
{
	GList *link, *last_headers = NULL;
	GSList *headers_stack = NULL;

	link = g_queue_peek_head_link (part_queue);
	while (link) {
		EMailPart *part = link->data;
		const gchar *id;

		if (!part) {
			link = g_list_next (link);
			continue;
		}

		id = e_mail_part_get_id (part);
		if (!id) {
			link = g_list_next (link);
			continue;
		}

		if (g_str_has_suffix (id, ".rfc822")) {
			headers_stack = g_slist_prepend (headers_stack, last_headers);
			last_headers = NULL;
		} else if (g_str_has_suffix (id, ".rfc822.end")) {
			g_warn_if_fail (headers_stack != NULL);

			if (headers_stack) {
				last_headers = headers_stack->data;
				headers_stack = g_slist_remove (headers_stack, last_headers);
			} else {
				last_headers = NULL;
			}
		}

		if (g_strcmp0 (e_mail_part_get_mime_type (part), "application/vnd.evolution.headers") == 0) {
			last_headers = link;
			link = g_list_next (link);
		} else if (g_strcmp0 (e_mail_part_get_mime_type (part), "application/vnd.evolution.secure-button") == 0) {
			g_warn_if_fail (last_headers != NULL);

			if (last_headers) {
				GList *next = g_list_next (link);

				g_warn_if_fail (g_queue_remove (part_queue, part));
				g_queue_insert_before (part_queue, last_headers, part);

				link = next;
			} else {
				link = g_list_next (link);
			}
		} else {
			link = g_list_next (link);
		}
	}

	g_warn_if_fail (headers_stack == NULL);
	g_slist_free (headers_stack);
}

static void
mail_parser_run (EMailParser *parser,
                 EMailPartList *part_list,
                 GCancellable *cancellable)
{
	EMailExtensionRegistry *reg;
	CamelMimeMessage *message;
	EMailPart *mail_part;
	GQueue *parsers;
	GQueue mail_part_queue = G_QUEUE_INIT;
	GList *iter;
	GString *part_id;

	if (cancellable)
		g_object_ref (cancellable);
	else
		cancellable = g_cancellable_new ();

	g_mutex_lock (&parser->priv->mutex);
	g_hash_table_insert (parser->priv->ongoing_part_lists, cancellable, part_list);
	g_mutex_unlock (&parser->priv->mutex);

	message = e_mail_part_list_get_message (part_list);

	reg = e_mail_parser_get_extension_registry (parser);

	parsers = e_mail_extension_registry_get_for_mime_type (
		reg, "application/vnd.evolution.message");

	if (parsers == NULL)
		parsers = e_mail_extension_registry_get_for_mime_type (
			reg, "message/*");

	/* No parsers means the internal Evolution parser
	 * extensions were not loaded. Something is terribly wrong! */
	g_return_if_fail (parsers != NULL);

	part_id = g_string_new (".message");

	mail_part = e_mail_part_new (CAMEL_MIME_PART (message), ".message");
	e_mail_part_list_add_part (part_list, mail_part);
	g_object_unref (mail_part);

	for (iter = parsers->head; iter; iter = iter->next) {
		EMailParserExtension *extension;
		gboolean message_handled;

		if (g_cancellable_is_cancelled (cancellable))
			break;

		extension = iter->data;
		if (!extension)
			continue;

		message_handled = e_mail_parser_extension_parse (
			extension, parser,
			CAMEL_MIME_PART (message),
			part_id, cancellable, &mail_part_queue);

		if (message_handled)
			break;
	}

	mail_parser_move_security_before_headers (&mail_part_queue);

	while (!g_queue_is_empty (&mail_part_queue)) {
		mail_part = g_queue_pop_head (&mail_part_queue);
		e_mail_part_list_add_part (part_list, mail_part);
		g_object_unref (mail_part);
	}

	g_mutex_lock (&parser->priv->mutex);
	g_hash_table_remove (parser->priv->ongoing_part_lists, cancellable);
	g_mutex_unlock (&parser->priv->mutex);

	g_clear_object (&cancellable);
	g_string_free (part_id, TRUE);
}

static void
shell_gone_cb (gpointer user_data,
	       GObject *gone_extension_registry)
{
	EMailParserClass *class = user_data;

	g_return_if_fail (class != NULL);

	g_clear_object (&class->extension_registry);
}

static void
mail_parser_set_session (EMailParser *parser,
                         CamelSession *session)
{
	g_return_if_fail (CAMEL_IS_SESSION (session));
	g_return_if_fail (parser->priv->session == NULL);

	parser->priv->session = g_object_ref (session);
}

static void
e_mail_parser_set_property (GObject *object,
                          guint property_id,
                          const GValue *value,
                          GParamSpec *pspec)
{
	switch (property_id) {
		case PROP_SESSION:
			mail_parser_set_session (
				E_MAIL_PARSER (object),
				g_value_get_object (value));
			return;
	}

	G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
}

static void
e_mail_parser_get_property (GObject *object,
                          guint property_id,
                          GValue *value,
                          GParamSpec *pspec)
{
	switch (property_id) {
		case PROP_SESSION:
			g_value_set_object (
				value,
				e_mail_parser_get_session (
				E_MAIL_PARSER (object)));
			return;
	}

	G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
}

static void
e_mail_parser_finalize (GObject *object)
{
	EMailParserPrivate *priv;

	priv = E_MAIL_PARSER_GET_PRIVATE (object);

	g_clear_object (&priv->session);
	g_hash_table_destroy (priv->ongoing_part_lists);
	g_mutex_clear (&priv->mutex);

	/* Chain up to parent's finalize() method. */
	G_OBJECT_CLASS (parent_class)->finalize (object);
}

static void
e_mail_parser_base_init (EMailParserClass *class)
{
	EShell *shell;

	/* Register internal extensions. */
	g_type_ensure (e_mail_parser_application_mbox_get_type ());
	/* This is currently disabled, because the WebKit player requires javascript,
	   which is disabled in Evolution. */
	/* g_type_ensure (e_mail_parser_audio_get_type ()); */
	g_type_ensure (e_mail_parser_headers_get_type ());
	g_type_ensure (e_mail_parser_message_get_type ());
	g_type_ensure (e_mail_parser_secure_button_get_type ());
	g_type_ensure (e_mail_parser_source_get_type ());
	g_type_ensure (e_mail_parser_image_get_type ());
	g_type_ensure (e_mail_parser_inline_pgp_encrypted_get_type ());
	g_type_ensure (e_mail_parser_inline_pgp_signed_get_type ());
	g_type_ensure (e_mail_parser_message_delivery_status_get_type ());
	g_type_ensure (e_mail_parser_message_external_get_type ());
	g_type_ensure (e_mail_parser_message_rfc822_get_type ());
	g_type_ensure (e_mail_parser_multipart_alternative_get_type ());
	g_type_ensure (e_mail_parser_multipart_apple_double_get_type ());
	g_type_ensure (e_mail_parser_multipart_digest_get_type ());
	g_type_ensure (e_mail_parser_multipart_encrypted_get_type ());
	g_type_ensure (e_mail_parser_multipart_mixed_get_type ());
	g_type_ensure (e_mail_parser_multipart_related_get_type ());
	g_type_ensure (e_mail_parser_multipart_signed_get_type ());
	g_type_ensure (e_mail_parser_text_enriched_get_type ());
	g_type_ensure (e_mail_parser_text_html_get_type ());
	g_type_ensure (e_mail_parser_text_plain_get_type ());
#ifdef ENABLE_SMIME
	g_type_ensure (e_mail_parser_application_smime_get_type ());
#endif

	class->extension_registry = g_object_new (
		E_TYPE_MAIL_PARSER_EXTENSION_REGISTRY, NULL);

	e_mail_parser_extension_registry_load (class->extension_registry);

	e_extensible_load_extensions (E_EXTENSIBLE (class->extension_registry));

	shell = e_shell_get_default ();
	/* It can be NULL when creating developer documentation */
	if (shell)
		g_object_weak_ref (G_OBJECT (shell), shell_gone_cb, class);
}

static void
e_mail_parser_class_init (EMailParserClass *class)
{
	GObjectClass *object_class;

	parent_class = g_type_class_peek_parent (class);
	g_type_class_add_private (class, sizeof (EMailParserPrivate));

	object_class = G_OBJECT_CLASS (class);
	object_class->finalize = e_mail_parser_finalize;
	object_class->set_property = e_mail_parser_set_property;
	object_class->get_property = e_mail_parser_get_property;

	g_object_class_install_property (
		object_class,
		PROP_SESSION,
		g_param_spec_object (
			"session",
			"Camel Session",
			NULL,
			CAMEL_TYPE_SESSION,
			G_PARAM_READWRITE |
			G_PARAM_CONSTRUCT_ONLY));
}

static void
e_mail_parser_init (EMailParser *parser)
{
	parser->priv = E_MAIL_PARSER_GET_PRIVATE (parser);
	parser->priv->ongoing_part_lists = g_hash_table_new (g_direct_hash, g_direct_equal);

	g_mutex_init (&parser->priv->mutex);
}

GType
e_mail_parser_get_type (void)
{
	static GType type = 0;

	if (G_UNLIKELY (type == 0)) {
		static const GTypeInfo type_info = {
			sizeof (EMailParserClass),
			(GBaseInitFunc) e_mail_parser_base_init,
			(GBaseFinalizeFunc) NULL,
			(GClassInitFunc) e_mail_parser_class_init,
			(GClassFinalizeFunc) NULL,
			NULL,  /* class_data */
			sizeof (EMailParser),
			0,     /* n_preallocs */
			(GInstanceInitFunc) e_mail_parser_init,
			NULL   /* value_table */
		};

		type = g_type_register_static (
			G_TYPE_OBJECT, "EMailParser",
			&type_info, 0);
	}

	return type;
}

EMailParser *
e_mail_parser_new (CamelSession *session)
{
	g_return_val_if_fail (CAMEL_IS_SESSION (session), NULL);

	return g_object_new (
		E_TYPE_MAIL_PARSER,
		"session", session, NULL);
}

/**
 * e_mail_parser_parse_sync:
 * @parser: an #EMailParser
 * @folder: (allow none) a #CamelFolder containing the @message or %NULL
 * @message_uid: (allow none) UID of the @message within the @folder or %NULL
 * @message: a #CamelMimeMessage
 * @cancellable: (allow-none) a #GCancellable
 *
 * Parses the @message synchronously. Returns a list of #EMailPart<!-- -->s which
 * represents structure of the message and additional properties of each part.
 *
 * Note that this function can block for a while, so it's not a good idea to call
 * it from main thread.
 *
 * Return Value: An #EMailPartsList
 */
EMailPartList *
e_mail_parser_parse_sync (EMailParser *parser,
                          CamelFolder *folder,
                          const gchar *message_uid,
                          CamelMimeMessage *message,
                          GCancellable *cancellable)
{
	EMailPartList *part_list;

	g_return_val_if_fail (E_IS_MAIL_PARSER (parser), NULL);
	g_return_val_if_fail (CAMEL_IS_MIME_MESSAGE (message), NULL);

	part_list = e_mail_part_list_new (message, message_uid, folder);

	mail_parser_run (parser, part_list, cancellable);

	if (camel_debug_start ("emformat:parser")) {
		GQueue queue = G_QUEUE_INIT;

		printf (
			"%s finished with EMailPartList:\n",
			G_OBJECT_TYPE_NAME (parser));

		e_mail_part_list_queue_parts (part_list, NULL, &queue);

		while (!g_queue_is_empty (&queue)) {
			EMailPart *part;

			part = g_queue_pop_head (&queue);

			printf (
				"	id: %s | cid: %s | mime_type: %s | "
				"is_hidden: %d | is_attachment: %d\n",
				e_mail_part_get_id (part),
				e_mail_part_get_cid (part),
				e_mail_part_get_mime_type (part),
				part->is_hidden ? 1 : 0,
				e_mail_part_get_is_attachment (part) ? 1 : 0);

			g_object_unref (part);
		}

		camel_debug_end ();
	}

	return part_list;
}

static void
mail_parser_parse_thread (GSimpleAsyncResult *simple,
                          GObject *source_object,
                          GCancellable *cancellable)
{
	EMailPartList *part_list;

	part_list = g_simple_async_result_get_op_res_gpointer (simple);

	mail_parser_run (
		E_MAIL_PARSER (source_object),
		part_list, cancellable);
}

/**
 * e_mail_parser_parse:
 * @parser: an #EMailParser
 * @message: a #CamelMimeMessage
 * @callback: a #GAsyncReadyCallback
 * @cancellable: (allow-none) a #GCancellable
 * @user_data: (allow-none) user data passed to the callback
 *
 * Asynchronous version of e_mail_parser_parse_sync().
 */
void
e_mail_parser_parse (EMailParser *parser,
                     CamelFolder *folder,
                     const gchar *message_uid,
                     CamelMimeMessage *message,
                     GAsyncReadyCallback callback,
                     GCancellable *cancellable,
                     gpointer user_data)
{
	GSimpleAsyncResult *simple;
	EMailPartList *part_list;

	g_return_if_fail (E_IS_MAIL_PARSER (parser));
	g_return_if_fail (CAMEL_IS_MIME_MESSAGE (message));

	part_list = e_mail_part_list_new (message, message_uid, folder);

	simple = g_simple_async_result_new (
		G_OBJECT (parser), callback,
		user_data, e_mail_parser_parse);

	g_simple_async_result_set_check_cancellable (simple, cancellable);

	g_simple_async_result_set_op_res_gpointer (
		simple, part_list, (GDestroyNotify) g_object_unref);

	g_simple_async_result_run_in_thread (
		simple, mail_parser_parse_thread,
		G_PRIORITY_DEFAULT, cancellable);

	g_object_unref (simple);
}

EMailPartList *
e_mail_parser_parse_finish (EMailParser *parser,
                            GAsyncResult *result,
                            GError **error)
{
	GSimpleAsyncResult *simple;
	EMailPartList *part_list;

	g_return_val_if_fail (
		g_simple_async_result_is_valid (
		result, G_OBJECT (parser), e_mail_parser_parse), NULL);

	simple = G_SIMPLE_ASYNC_RESULT (result);
	part_list = g_simple_async_result_get_op_res_gpointer (simple);

	if (camel_debug_start ("emformat:parser")) {
		GQueue queue = G_QUEUE_INIT;

		printf (
			"%s finished with EMailPartList:\n",
			G_OBJECT_TYPE_NAME (parser));

		e_mail_part_list_queue_parts (part_list, NULL, &queue);

		while (!g_queue_is_empty (&queue)) {
			EMailPart *part;

			part = g_queue_pop_head (&queue);

			printf (
				"	id: %s | cid: %s | mime_type: %s | "
				"is_hidden: %d | is_attachment: %d\n",
				e_mail_part_get_id (part),
				e_mail_part_get_cid (part),
				e_mail_part_get_mime_type (part),
				part->is_hidden ? 1 : 0,
				e_mail_part_get_is_attachment (part) ? 1 : 0);

			g_object_unref (part);
		}

		camel_debug_end ();
	}

	return g_object_ref (part_list);
}

GQueue *
e_mail_parser_get_parsers_for_part (EMailParser *parser,
				    CamelMimePart *part)
{
	CamelContentType *ct;
	gchar *mime_type;
	GQueue *parsers;

	g_return_val_if_fail (E_IS_MAIL_PARSER (parser), NULL);
	g_return_val_if_fail (CAMEL_IS_MIME_PART (part), NULL);

	ct = camel_mime_part_get_content_type (part);
	if (!ct) {
		mime_type = (gchar *) "application/vnd.evolution.error";
	} else {
		gchar *tmp;
		tmp = camel_content_type_simple (ct);
		mime_type = g_ascii_strdown (tmp, -1);
		g_free (tmp);
	}

	parsers = e_mail_parser_get_parsers (parser, mime_type);

	if (ct)
		g_free (mime_type);

	return parsers;
}

GQueue *
e_mail_parser_get_parsers (EMailParser *parser,
			   const gchar *mime_type)
{
	EMailExtensionRegistry *reg;
	EMailParserClass *parser_class;
	gchar *as_mime_type;
	GQueue *parsers;

	g_return_val_if_fail (E_IS_MAIL_PARSER (parser), NULL);

	parser_class = E_MAIL_PARSER_GET_CLASS (parser);
	g_return_val_if_fail (parser_class != NULL, NULL);

	if (mime_type)
		as_mime_type = g_ascii_strdown (mime_type, -1);
	else
		as_mime_type = NULL;

	reg = E_MAIL_EXTENSION_REGISTRY (parser_class->extension_registry);

	parsers = e_mail_extension_registry_get_for_mime_type (reg, as_mime_type);
	if (!parsers)
		parsers = e_mail_extension_registry_get_fallback (reg, as_mime_type);

	g_free (as_mime_type);

	return parsers;
}

gboolean
e_mail_parser_parse_part (EMailParser *parser,
                          CamelMimePart *part,
                          GString *part_id,
                          GCancellable *cancellable,
                          GQueue *out_mail_parts)
{
	CamelContentType *ct;
	gchar *mime_type;
	gint handled;

	ct = camel_mime_part_get_content_type (part);
	if (!ct) {
		mime_type = (gchar *) "application/vnd.evolution.error";
	} else {
		gchar *tmp;
		tmp = camel_content_type_simple (ct);
		mime_type = g_ascii_strdown (tmp, -1);
		g_free (tmp);
	}

	handled = e_mail_parser_parse_part_as (
		parser, part, part_id, mime_type,
		cancellable, out_mail_parts);

	if (ct) {
		g_free (mime_type);
	}

	return handled;
}

gboolean
e_mail_parser_parse_part_as (EMailParser *parser,
                             CamelMimePart *part,
                             GString *part_id,
                             const gchar *mime_type,
                             GCancellable *cancellable,
                             GQueue *out_mail_parts)
{
	GQueue *parsers;
	GList *iter;
	gboolean mime_part_handled = FALSE;

	parsers = e_mail_parser_get_parsers (parser, mime_type);

	if (parsers == NULL) {
		e_mail_parser_wrap_as_attachment (
			parser, part, part_id, out_mail_parts);
		return TRUE;
	}

	for (iter = parsers->head; iter; iter = iter->next) {
		EMailParserExtension *extension;

		extension = iter->data;
		if (!extension)
			continue;

		mime_part_handled = e_mail_parser_extension_parse (
			extension, parser, part, part_id,
			cancellable, out_mail_parts);

		if (mime_part_handled)
			break;
	}

	return mime_part_handled;
}

void
e_mail_parser_error (EMailParser *parser,
                     GQueue *out_mail_parts,
                     const gchar *format,
                     ...)
{
	const gchar *mime_type = "application/vnd.evolution.error";
	EMailPart *mail_part;
	CamelMimePart *part;
	gchar *errmsg;
	gchar *uri;
	va_list ap;

	g_return_if_fail (E_IS_MAIL_PARSER (parser));
	g_return_if_fail (out_mail_parts != NULL);
	g_return_if_fail (format != NULL);

	va_start (ap, format);
	errmsg = g_strdup_vprintf (format, ap);

	part = camel_mime_part_new ();
	camel_mime_part_set_content (
		part, errmsg, strlen (errmsg), mime_type);
	g_free (errmsg);
	va_end (ap);

	g_mutex_lock (&parser->priv->mutex);
	parser->priv->last_error++;
	uri = g_strdup_printf (".error.%d", parser->priv->last_error);
	g_mutex_unlock (&parser->priv->mutex);

	mail_part = e_mail_part_new (part, uri);
	e_mail_part_set_mime_type (mail_part, mime_type);
	mail_part->is_error = TRUE;

	g_free (uri);
	g_object_unref (part);

	g_queue_push_tail (out_mail_parts, mail_part);
}

static void
attachment_loaded (EAttachment *attachment,
                   GAsyncResult *res,
                   gpointer user_data)
{
	EShell *shell;
	GtkWindow *window;

	shell = e_shell_get_default ();
	window = e_shell_get_active_window (shell);

	e_attachment_load_handle_error (attachment, res, window);

	g_object_unref (attachment);
}

/* Idle callback */
static gboolean
load_attachment_idle (EAttachment *attachment)
{
	e_attachment_load_async (
		attachment,
		(GAsyncReadyCallback) attachment_loaded, NULL);

	return FALSE;
}

void
e_mail_parser_wrap_as_attachment (EMailParser *parser,
                                  CamelMimePart *part,
                                  GString *part_id,
                                  GQueue *parts_queue)
{
	EMailPartAttachment *empa;
	EAttachment *attachment;
	EMailPart *first_part;
	const gchar *snoop_mime_type;
	GQueue *extensions;
	CamelContentType *ct;
	gchar *mime_type;
	CamelDataWrapper *dw;
	GByteArray *ba;
	gsize size;
	gint part_id_len;

	ct = camel_mime_part_get_content_type (part);
	extensions = NULL;
	snoop_mime_type = NULL;
	if (ct) {
		EMailExtensionRegistry *reg;
		mime_type = camel_content_type_simple (ct);

		reg = e_mail_parser_get_extension_registry (parser);
		extensions = e_mail_extension_registry_get_for_mime_type (
			reg, mime_type);

		if (camel_content_type_is (ct, "text", "*") ||
		    camel_content_type_is (ct, "message", "*"))
			snoop_mime_type = mime_type;
		else
			g_free (mime_type);
	}

	if (!snoop_mime_type)
		snoop_mime_type = e_mail_part_snoop_type (part);

	if (!extensions) {
		EMailExtensionRegistry *reg;

		reg = e_mail_parser_get_extension_registry (parser);
		extensions = e_mail_extension_registry_get_for_mime_type (
			reg, snoop_mime_type);

		if (!extensions) {
			extensions = e_mail_extension_registry_get_fallback (
				reg, snoop_mime_type);
		}
	}

	part_id_len = part_id->len;
	g_string_append (part_id, ".attachment");

	empa = e_mail_part_attachment_new (part, part_id->str);
	empa->shown = extensions && (!g_queue_is_empty (extensions) &&
		e_mail_part_is_inline (part, extensions));
	empa->snoop_mime_type = snoop_mime_type;

	first_part = g_queue_peek_head (parts_queue);
	if (first_part != NULL && !E_IS_MAIL_PART_ATTACHMENT (first_part)) {
		const gchar *id = e_mail_part_get_id (first_part);
		empa->part_id_with_attachment = g_strdup (id);
		first_part->is_hidden = TRUE;
	}

	attachment = e_mail_part_attachment_ref_attachment (empa);

	e_attachment_set_initially_shown (attachment, empa->shown);
	e_attachment_set_can_show (
		attachment,
		extensions && !g_queue_is_empty (extensions));

	/* Try to guess size of the attachments */
	dw = camel_medium_get_content (CAMEL_MEDIUM (part));
	ba = camel_data_wrapper_get_byte_array (dw);
	if (ba) {
		size = ba->len;

		if (camel_mime_part_get_encoding (part) == CAMEL_TRANSFER_ENCODING_BASE64)
			size = size / 1.37;
	} else {
		size = 0;
	}

	/* e_attachment_load_async must be called from main thread */
	/* Prioritize ahead of GTK+ redraws. */
	g_idle_add_full (
		G_PRIORITY_HIGH_IDLE,
		(GSourceFunc) load_attachment_idle,
		g_object_ref (attachment),
		NULL);

	if (size != 0) {
		GFileInfo *file_info;

		file_info = e_attachment_ref_file_info (attachment);

		if (file_info == NULL) {
			file_info = g_file_info_new ();
			g_file_info_set_content_type (
				file_info, empa->snoop_mime_type);
		}

		g_file_info_set_size (file_info, size);
		e_attachment_set_file_info (attachment, file_info);

		g_object_unref (file_info);
	}

	g_object_unref (attachment);

	g_string_truncate (part_id, part_id_len);

	/* Push to head, not tail. */
	g_queue_push_head (parts_queue, empa);
}

void
e_mail_parser_wrap_as_non_expandable_attachment (EMailParser *parser,
						 CamelMimePart *part,
						 GString *part_id,
						 GQueue *out_parts_queue)
{
	GQueue work_queue = G_QUEUE_INIT;
	GList *head, *link;

	g_return_if_fail (E_IS_MAIL_PARSER (parser));
	g_return_if_fail (CAMEL_IS_MIME_PART (part));
	g_return_if_fail (part_id != NULL);
	g_return_if_fail (out_parts_queue != NULL);

	e_mail_parser_wrap_as_attachment (parser, part, part_id, &work_queue);

	head = g_queue_peek_head_link (&work_queue);

	for (link = head; link; link = g_list_next (link)) {
		EMailPartAttachment *empa = link->data;

		if (E_IS_MAIL_PART_ATTACHMENT (empa)) {
			EAttachment *attachment;
			CamelMimePart *att_part;

			empa->shown = FALSE;
			e_mail_part_attachment_set_expandable (empa, FALSE);

			attachment = e_mail_part_attachment_ref_attachment (empa);
			e_attachment_set_initially_shown (attachment, FALSE);
			e_attachment_set_can_show (attachment, FALSE);

			att_part = e_attachment_ref_mime_part (attachment);
			if (att_part)
				camel_mime_part_set_disposition (att_part, NULL);

			g_clear_object (&att_part);
			g_clear_object (&attachment);
		}
	}

	e_queue_transfer (&work_queue, out_parts_queue);
}

CamelSession *
e_mail_parser_get_session (EMailParser *parser)
{
	g_return_val_if_fail (E_IS_MAIL_PARSER (parser), NULL);

	return parser->priv->session;
}

/* The 'operation' is not used as a GCancellable, but as an identificator
   of an ongoing operation for which the part list is requested. */
EMailPartList *
e_mail_parser_ref_part_list_for_operation (EMailParser *parser,
					   GCancellable *operation)
{
	EMailPartList *part_list;

	g_return_val_if_fail (E_IS_MAIL_PARSER (parser), NULL);

	g_mutex_lock (&parser->priv->mutex);
	part_list = g_hash_table_lookup (parser->priv->ongoing_part_lists, operation);
	if (part_list)
		g_object_ref (part_list);
	g_mutex_unlock (&parser->priv->mutex);

	return part_list;
}

EMailExtensionRegistry *
e_mail_parser_get_extension_registry (EMailParser *parser)
{
	EMailParserClass *parser_class;

	g_return_val_if_fail (E_IS_MAIL_PARSER (parser), NULL);

	parser_class = E_MAIL_PARSER_GET_CLASS (parser);
	g_return_val_if_fail (parser_class != NULL, NULL);

	return E_MAIL_EXTENSION_REGISTRY (parser_class->extension_registry);
}
