/* -*- Mode: C; indent-tabs-mode: t; c-basic-offset: 8; tab-width: 8 -*- */
/* e-ldap-storage.c
 *
 * Copyright (C) 2000  Ximian, Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 *
 * Author: Chris Toshok
 */

/* The addressbook-sources.xml file goes like this:

   <?xml version="1.0"?>
   <addressbooks>
     <contactserver>
           <name>LDAP Server</name>
	   <host>ldap.server.com</host>
	   <port>389</port>
	   <rootdn></rootdn>
	   <authmethod>simple</authmethod>
	   <emailaddr>toshok@blubag.com</emailaddr>
	   <rememberpass/>
     </contactserver>
   </addressbooks>
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <sys/types.h>
#include <sys/fcntl.h>
#include <sys/stat.h>
#include <errno.h>

#include <glib.h>
#include <gtk/gtk.h>
#include <gnome-xml/parser.h>
#include <gnome-xml/xmlmemory.h>

#include <libgnome/gnome-defs.h>
#include <libgnome/gnome-i18n.h>
#include <libgnome/gnome-util.h>
#include <bonobo/bonobo-object.h>

#include <gal/util/e-util.h>
#include <gal/util/e-xml-utils.h>
#include <libgnome/gnome-i18n.h>

#include "e-util/e-unicode-i18n.h"

#include "evolution-shell-component.h"
#include "evolution-storage.h"

#include "addressbook-storage.h"

#define ADDRESSBOOK_SOURCES_XML "addressbook-sources.xml"

static gboolean load_source_data (const char *file_path);
static gboolean save_source_data (const char *file_path);
static void deregister_storage (void);

static GList *sources;
static EvolutionStorage *storage;
static char *storage_path;
static GNOME_Evolution_Shell corba_shell;

void
addressbook_storage_setup (EvolutionShellComponent *shell_component,
			   const char *evolution_homedir)
{
	EvolutionShellClient *shell_client;

	shell_client = evolution_shell_component_get_owner (shell_component);
	if (shell_client == CORBA_OBJECT_NIL) {
		g_warning ("We have no shell!?");
		return;
	}

	corba_shell = bonobo_object_corba_objref (BONOBO_OBJECT (shell_client));

	sources = NULL;

	if (storage_path)
		g_free (storage_path);
	storage_path = g_concat_dir_and_file (evolution_homedir, ADDRESSBOOK_SOURCES_XML);
	if (!load_source_data (storage_path))
		deregister_storage ();
}

#ifdef HAVE_LDAP
static int
remove_ldap_folder (EvolutionStorage *storage,
		    const CORBA_char *path, const CORBA_char *physical_uri,
		    gpointer data)
{
	addressbook_storage_remove_source (path + 1);
	addressbook_storage_write_sources();
	return GNOME_Evolution_Storage_OK;
}
static int
create_ldap_folder (EvolutionStorage *storage,
		    const CORBA_char *path, const CORBA_char *type,
		    const CORBA_char *description, const CORBA_char *parent_physical_uri,
		    int *result, gpointer data)
{
	if (strcmp (type, "contacts"))
		return GNOME_Evolution_Storage_UNSUPPORTED_TYPE;

	if (strcmp (parent_physical_uri, "")) /* ldap servers can't have subfolders */
		return GNOME_Evolution_Storage_INVALID_URI;

	addressbook_create_new_source (path + 1, NULL);

	return GNOME_Evolution_Storage_OK;
}
#endif


EvolutionStorage *
addressbook_get_other_contact_storage (void) 
{
#ifdef HAVE_LDAP
	EvolutionStorageResult result;

	if (storage == NULL) {
		storage = evolution_storage_new (U_("Other Contacts"), NULL, NULL);
		gtk_signal_connect (GTK_OBJECT (storage),
				    "remove_folder",
				    GTK_SIGNAL_FUNC(remove_ldap_folder), NULL);
		gtk_signal_connect (GTK_OBJECT (storage),
				    "create_folder",
				    GTK_SIGNAL_FUNC(create_ldap_folder), NULL);
		result = evolution_storage_register_on_shell (storage, corba_shell);
		switch (result) {
		case EVOLUTION_STORAGE_OK:
			break;
		case EVOLUTION_STORAGE_ERROR_GENERIC : 
			g_warning("register_storage: generic error");
			break;
		case EVOLUTION_STORAGE_ERROR_CORBA : 
			g_warning("register_storage: corba error");
			break;
		case EVOLUTION_STORAGE_ERROR_ALREADYREGISTERED :
			g_warning("register_storage: already registered error");
			break;
		case EVOLUTION_STORAGE_ERROR_EXISTS :
			g_warning("register_storage: already exists error");
			break;
		default:
			g_warning("register_storage: other error");
			break;
		}
	}
#endif

	return storage;
}

static void 
deregister_storage (void)
{
	if (evolution_storage_deregister_on_shell (storage, corba_shell) != 
	    EVOLUTION_STORAGE_OK) {
		g_warning("couldn't deregister storage");
	}

	storage = NULL;
}

static char *
get_string_value (xmlNode *node,
		  const char *name)
{
	xmlNode *p;
	xmlChar *xml_string;
	char *retval;

	p = e_xml_get_child_by_name (node, (xmlChar *) name);
	if (p == NULL)
		return NULL;

	p = e_xml_get_child_by_name (p, (xmlChar *) "text");
	if (p == NULL) /* there's no text between the tags, return the empty string */
		return g_strdup("");

	xml_string = xmlNodeListGetString (node->doc, p, 1);
	retval = g_strdup ((char *) xml_string);
	xmlFree (xml_string);

	return retval;
}

static char *
ldap_unparse_auth (AddressbookLDAPAuthType auth_type)
{
	switch (auth_type) {
	case ADDRESSBOOK_LDAP_AUTH_NONE:
		return "none";
	case ADDRESSBOOK_LDAP_AUTH_SIMPLE:
		return "simple";
	default:
		g_assert(0);
		return "none";
	}
}

static AddressbookLDAPAuthType
ldap_parse_auth (const char *auth)
{
	if (!auth)
		return ADDRESSBOOK_LDAP_AUTH_NONE;

	if (!strcmp (auth, "simple"))
		return ADDRESSBOOK_LDAP_AUTH_SIMPLE;
	else
		return ADDRESSBOOK_LDAP_AUTH_NONE;
}

static char *
ldap_unparse_scope (AddressbookLDAPScopeType scope_type)
{
	switch (scope_type) {
	case ADDRESSBOOK_LDAP_SCOPE_BASE:
		return "base";
	case ADDRESSBOOK_LDAP_SCOPE_ONELEVEL:
		return "one";
	case ADDRESSBOOK_LDAP_SCOPE_SUBTREE:
		return "sub";
	default:
		g_assert(0);
		return "";
	}
}

static AddressbookLDAPScopeType
ldap_parse_scope (const char *scope)
{
	if (!scope)
		return ADDRESSBOOK_LDAP_SCOPE_SUBTREE; /* XXX good default? */

	if (!strcmp (scope, "base"))
		return ADDRESSBOOK_LDAP_SCOPE_BASE;
	else if (!strcmp (scope, "one"))
		return ADDRESSBOOK_LDAP_SCOPE_ONELEVEL;
	else
		return ADDRESSBOOK_LDAP_SCOPE_SUBTREE;
}

void
addressbook_storage_init_source_uri (AddressbookSource *source)
{
	if (source->uri)
		g_free (source->uri);

	source->uri = g_strdup_printf  ("ldap://%s:%s/%s??%s",
					source->host, source->port,
					source->rootdn, ldap_unparse_scope(source->scope));
}

static gboolean
load_source_data (const char *file_path)
{
	xmlDoc *doc;
	xmlNode *root;
	xmlNode *child;

	addressbook_get_other_contact_storage();

 tryagain:
	doc = xmlParseFile (file_path);
	if (doc == NULL) {
		/* Check to see if a addressbook-sources.xml.new file
                   exists.  If it does, rename it and try loading it */
		char *new_path = g_strdup_printf ("%s.new", file_path);
		struct stat sb;

		if (stat (new_path, &sb) == 0) {
			int rv;

			rv = rename (new_path, file_path);
			g_free (new_path);

			if (rv < 0) {
				g_error ("Failed to rename %s: %s\n",
					 ADDRESSBOOK_SOURCES_XML,
					 strerror(errno));
				return FALSE;
			} else
				goto tryagain;
		}

		g_free (new_path);
		return FALSE;
	}

	root = xmlDocGetRootElement (doc);
	if (root == NULL || strcmp (root->name, "addressbooks") != 0) {
		xmlFreeDoc (doc);
		return FALSE;
	}

	for (child = root->childs; child; child = child->next) {
		char *path;
		AddressbookSource *source;

		source = g_new0 (AddressbookSource, 1);

		if (!strcmp (child->name, "contactserver")) {
			source->type        = ADDRESSBOOK_SOURCE_LDAP;
			source->port   = get_string_value (child, "port");
			source->host   = get_string_value (child, "host");
			source->rootdn = get_string_value (child, "rootdn");
			source->scope  = ldap_parse_scope (get_string_value (child, "scope"));
			source->auth   = ldap_parse_auth (get_string_value (child, "authmethod"));
			source->email_addr = get_string_value (child, "emailaddr");
		}
		else {
			g_warning ("unknown node '%s' in %s", child->name, file_path);
			g_free (source);
			continue;
		}

		addressbook_storage_init_source_uri (source);

		source->name = get_string_value (child, "name");
		source->description = get_string_value (child, "description");

		path = g_strdup_printf ("/%s", source->name);
		evolution_storage_new_folder (storage, path, source->name,
					      "contacts", source->uri,
					      source->description, 0);

		sources = g_list_append (sources, source);

		g_free (path);
	}

	if (g_list_length (sources) == 0)
		deregister_storage();

	xmlFreeDoc (doc);
	return TRUE;
}

static void
ldap_source_foreach(AddressbookSource *source, xmlNode *root)
{
	xmlNode *source_root = xmlNewNode (NULL,
					   (xmlChar *) "contactserver");

	xmlAddChild (root, source_root);

	xmlNewChild (source_root, NULL, (xmlChar *) "name",
		     (xmlChar *) source->name);
	xmlNewChild (source_root, NULL, (xmlChar *) "description",
		     (xmlChar *) source->description);

	xmlNewChild (source_root, NULL, (xmlChar *) "port",
		     (xmlChar *) source->port);
	xmlNewChild (source_root, NULL, (xmlChar *) "host",
		     (xmlChar *) source->host);
	xmlNewChild (source_root, NULL, (xmlChar *) "rootdn",
		     (xmlChar *) source->rootdn);
	xmlNewChild (source_root, NULL, (xmlChar *) "scope",
		     (xmlChar *) ldap_unparse_scope(source->scope));
	xmlNewChild (source_root, NULL, (xmlChar *) "authmethod",
		     (xmlChar *) ldap_unparse_auth(source->auth));
	if (source->auth == ADDRESSBOOK_LDAP_AUTH_SIMPLE) {
		xmlNewChild (source_root, NULL, (xmlChar *) "emailaddr",
			     (xmlChar *) source->email_addr);
		if (source->remember_passwd)
			xmlNewChild (source_root, NULL, (xmlChar *) "rememberpass",
				     NULL);
	}
}

static gboolean
save_source_data (const char *file_path)
{
	xmlDoc *doc;
	xmlNode *root;
	int fd, rv;
	xmlChar *buf;
	int buf_size;
	char *new_path = g_strdup_printf ("%s.new", file_path);

	doc = xmlNewDoc ((xmlChar *) "1.0");
	root = xmlNewDocNode (doc, NULL, (xmlChar *) "addressbooks", NULL);
	xmlDocSetRootElement (doc, root);

	g_list_foreach (sources, (GFunc)ldap_source_foreach, root);

	fd = open (new_path, O_CREAT | O_TRUNC | O_WRONLY, 0600);
	fchmod (fd, 0600);

	xmlDocDumpMemory (doc, &buf, &buf_size);

	if (buf == NULL) {
		g_error ("Failed to write %s: xmlBufferCreate() == NULL", ADDRESSBOOK_SOURCES_XML);
		return FALSE;
	}

	rv = write (fd, buf, buf_size);
	xmlFree (buf);
	close (fd);

	if (0 > rv) {
		g_error ("Failed to write new %s: %s\n", ADDRESSBOOK_SOURCES_XML, strerror(errno));
		unlink (new_path);
		return FALSE;
	}
	else {
		if (0 > rename (new_path, file_path)) {
			g_error ("Failed to rename %s: %s\n", ADDRESSBOOK_SOURCES_XML, strerror(errno));
			unlink (new_path);
			return FALSE;
		}
		return TRUE;
	}
}

void
addressbook_storage_add_source (AddressbookSource *source)
{
	char *path;

	sources = g_list_append (sources, source);

	/* And then to the ui */
	addressbook_get_other_contact_storage();
	path = g_strdup_printf ("/%s", source->name);
	evolution_storage_new_folder (storage, path, source->name, "contacts",
				      source->uri, source->description, 0);

	g_free (path);
}

void
addressbook_storage_remove_source (const char *name)
{
	char *path;
	AddressbookSource *source = NULL;
	GList *l;

	/* remove it from our hashtable */
	for (l = sources; l; l = l->next) {
		AddressbookSource *s = l->data;
		if (!strcmp (s->name, name)) {
			source = s;
			break;
		}
	}

	if (!source)
		return;

	sources = g_list_remove_link (sources, l);
	g_list_free_1 (l);

	addressbook_source_free (source);

	/* and then from the ui */
	path = g_strdup_printf ("/%s", name);
	evolution_storage_removed_folder (storage, path);

	if (g_list_length (sources) == 0) 
		deregister_storage ();

	g_free (path);
}

GList *
addressbook_storage_get_sources ()
{
	return sources;
}

AddressbookSource *
addressbook_storage_get_source_by_uri (const char *uri)
{
	GList *l;

	for (l = sources; l ; l = l->next) {
		AddressbookSource *source = l->data;
		if (!strcmp (uri, source->uri))
			return source;
	}

	return NULL;
}

void
addressbook_source_free (AddressbookSource *source)
{
	g_free (source->name);
	g_free (source->description);
	g_free (source->uri);
	g_free (source->host);
	g_free (source->port);
	g_free (source->rootdn);
	g_free (source->email_addr);

	g_free (source);
}

static void
addressbook_source_foreach (AddressbookSource *source, gpointer data)
{
	char *path = g_strdup_printf ("/%s", source->name);

	evolution_storage_removed_folder (storage, path);

	g_free (path);

	addressbook_source_free (source);
}

void
addressbook_storage_clear_sources (void)
{
	g_list_foreach (sources, (GFunc)addressbook_source_foreach, NULL);
	g_list_free (sources);
	deregister_storage ();
	sources = NULL;
}

void
addressbook_storage_write_sources (void)
{
	save_source_data (storage_path);
}

AddressbookSource *
addressbook_source_copy (const AddressbookSource *source)
{
	AddressbookSource *copy;

	copy = g_new0 (AddressbookSource, 1);
	copy->name = g_strdup (source->name);
	copy->description = g_strdup (source->description);
	copy->type = source->type;
	copy->uri = g_strdup (source->uri);

	copy->host = g_strdup (source->host);
	copy->port = g_strdup (source->port);
	copy->rootdn = g_strdup (source->rootdn);
	copy->scope = source->scope;
	copy->auth = source->auth;
	copy->email_addr = g_strdup (source->email_addr);
	copy->remember_passwd = source->remember_passwd;

	return copy;
}
