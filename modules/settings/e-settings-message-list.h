/*
 * e-settings-message-list.h
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

#ifndef E_SETTINGS_MESSAGE_LIST_H
#define E_SETTINGS_MESSAGE_LIST_H

#include <libebackend/libebackend.h>

/* Standard GObject macros */
#define E_TYPE_SETTINGS_MESSAGE_LIST \
	(e_settings_message_list_get_type ())
#define E_SETTINGS_MESSAGE_LIST(obj) \
	(G_TYPE_CHECK_INSTANCE_CAST \
	((obj), E_TYPE_SETTINGS_MESSAGE_LIST, ESettingsMessageList))
#define E_SETTINGS_MESSAGE_LIST_CLASS(cls) \
	(G_TYPE_CHECK_CLASS_CAST \
	((cls), E_TYPE_SETTINGS_MESSAGE_LIST, ESettingsMessageListClass))
#define E_IS_SETTINGS_MESSAGE_LIST(obj) \
	(G_TYPE_CHECK_INSTANCE_TYPE \
	((obj), E_TYPE_SETTINGS_MESSAGE_LIST))
#define E_IS_SETTINGS_MESSAGE_LIST_CLASS(cls) \
	(G_TYPE_CHECK_CLASS_TYPE \
	((cls), E_TYPE_SETTINGS_MESSAGE_LIST))
#define E_SETTINGS_MESSAGE_LIST_GET_CLASS(obj) \
	(G_TYPE_INSTANCE_GET_CLASS \
	((obj), E_TYPE_SETTINGS_MESSAGE_LIST, ESettingsMessageListClass))

G_BEGIN_DECLS

typedef struct _ESettingsMessageList ESettingsMessageList;
typedef struct _ESettingsMessageListClass ESettingsMessageListClass;
typedef struct _ESettingsMessageListPrivate ESettingsMessageListPrivate;

struct _ESettingsMessageList {
	EExtension parent;
	ESettingsMessageListPrivate *priv;
};

struct _ESettingsMessageListClass {
	EExtensionClass parent_class;
};

GType		e_settings_message_list_get_type
						(void) G_GNUC_CONST;
void		e_settings_message_list_type_register
						(GTypeModule *type_module);

G_END_DECLS

#endif /* E_SETTINGS_MESSAGE_LIST_H */

