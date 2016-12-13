/*
 * Copyright © 2016 Red Hat, Inc
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 *
 * Authors:
 *       Alexander Larsson <alexl@redhat.com>
 */

#include "config.h"

#include <glib/gi18n.h>

#include "flatpak-transaction.h"
#include "flatpak-utils.h"
#include "flatpak-builtins-utils.h"
#include "flatpak-oci-registry.h"
#include "flatpak-error.h"

typedef struct FlatpakTransactionOp FlatpakTransactionOp;

struct FlatpakTransactionOp {
  char *remote;
  char *ref;
  char **subpaths;
  char *commit;
  gboolean update;
  gboolean install;
  gboolean non_fatal;
};

struct FlatpakTransaction {
  FlatpakDir *dir;
  GHashTable *refs;
  GList *ops;

  gboolean no_pull;
  gboolean no_deploy;
  gboolean add_deps;
  gboolean add_related;
};


/* Check if the ref is in the dir, or in the system dir, in case its a
 * user-dir. We want to avoid depending on user-installed things when
 * installing to the system dir.
 */
static gboolean
ref_is_installed (FlatpakDir *dir, const char *ref)
{
  g_autoptr(GFile) deploy_dir = NULL;

  deploy_dir = flatpak_dir_get_if_deployed (dir, ref, NULL, NULL);
  if (deploy_dir != NULL)
    return TRUE;

  if (flatpak_dir_is_user (dir))
    {
      g_autoptr(FlatpakDir) system_dir = flatpak_dir_get_system ();

      deploy_dir = flatpak_dir_get_if_deployed (system_dir, ref, NULL, NULL);
      if (deploy_dir != NULL)
        return TRUE;
    }

  return FALSE;
}

static gboolean
dir_ref_is_installed (FlatpakDir *dir, const char *ref, char **remote_out)
{
  g_autoptr(GVariant) deploy_data = NULL;

  deploy_data = flatpak_dir_get_deploy_data (dir, ref, NULL, NULL);
  if (deploy_data == NULL)
    return FALSE;

  if (remote_out)
    *remote_out = g_strdup (flatpak_deploy_data_get_origin (deploy_data));
  return TRUE;
}

static char *
transaction_fetch_runtime_ref (FlatpakDir *dir, const char *remote, const char *ref)
{
  g_autofree char *metadata = NULL;
  g_autoptr(GKeyFile) metakey = NULL;
  g_autofree char *runtime_ref = NULL;

  if (!g_str_has_prefix (ref, "app/"))
    return NULL;

  if (!flatpak_dir_fetch_ref_cache (dir, remote, ref, NULL, NULL, &metadata, NULL, NULL))
    return NULL;

  metakey = g_key_file_new ();
  if (!g_key_file_load_from_data (metakey, metadata, -1, 0, NULL))
    return NULL;

  runtime_ref = g_key_file_get_string (metakey, "Application", "runtime", NULL);

  return g_steal_pointer (&runtime_ref);
}


static FlatpakTransactionOp *
flatpak_transaction_operation_new (const char *remote,
                                   const char *ref,
                                   const char **subpaths,
                                   const char *commit,
                                   gboolean install,
                                   gboolean update)
{
  FlatpakTransactionOp *self = g_new0 (FlatpakTransactionOp, 1);

  self->remote = g_strdup (remote);
  self->ref = g_strdup (ref);
  self->subpaths = g_strdupv ((char **)subpaths);
  self->commit = g_strdup (commit);
  self->update = update;
  self->install = install;

  return self;
}

static void
flatpak_transaction_operation_free (FlatpakTransactionOp *self)
{
  g_free (self->remote);
  g_free (self->ref);
  g_free (self->commit);
  g_strfreev (self->subpaths);
  g_free (self);
}

FlatpakTransaction *
flatpak_transaction_new (FlatpakDir *dir,
                         gboolean no_pull,
                         gboolean no_deploy,
                         gboolean add_deps,
                         gboolean add_related)
{
  FlatpakTransaction *t = g_new0 (FlatpakTransaction, 1);

  t->dir = g_object_ref (dir);
  t->refs = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

  t->no_pull = no_pull;
  t->no_deploy = no_deploy;
  t->add_deps = add_deps;
  t->add_related = add_related;
  return t;
}

void
flatpak_transaction_free (FlatpakTransaction *self)
{
  g_hash_table_unref (self->refs);
  g_list_free_full (self->ops, (GDestroyNotify)flatpak_transaction_operation_free);
  g_object_unref (self->dir);
  g_free (self);
}

gboolean
flatpak_transaction_contains_ref (FlatpakTransaction *self,
                                  const char *ref)
{
  FlatpakTransactionOp *op;

  op = g_hash_table_lookup (self->refs, ref);

  return op != NULL;
}


static char *
subpaths_to_string (const char **subpaths)
{
  GString *s = NULL;
  int i;

  if (subpaths == NULL)
    return g_strdup ("[$old]");

  if (*subpaths == 0)
    return g_strdup ("[*]");

  s = g_string_new ("[");
  for (i = 0; subpaths[i] != NULL; i++)
    {
      if (i != 0)
        g_string_append (s, ", ");
      g_string_append (s, subpaths[i]);
    }
  g_string_append (s, "]");

  return g_string_free (s, FALSE);
}

FlatpakTransactionOp *
flatpak_transaction_add_op (FlatpakTransaction *self,
                            const char *remote,
                            const char *ref,
                            const char **subpaths,
                            const char *commit,
                            gboolean install,
                            gboolean update)
{
  FlatpakTransactionOp *op;
  g_autofree char *subpaths_str = NULL;
  const char *opname;

  if (install)
    {
      if (update)
        opname = "install/update";
      else
        opname = "install";
    }
  else
    opname = "update";
  subpaths_str = subpaths_to_string (subpaths);
  g_debug ("Transaction: %s %s:%s%s%s%s",
           opname, remote, ref,
           commit != NULL ? "@" : "",
           commit != NULL ? commit : "",
           subpaths_str);

  op = g_hash_table_lookup (self->refs, ref);
  if (op != NULL)
    {
      /* Only override subpaths if already specified,
         we always want the un-subpathed to win if specified. */
      if (op->subpaths != NULL && op->subpaths[0] != NULL && subpaths != NULL)
        {
          g_strfreev (op->subpaths);
          op->subpaths = g_strdupv ((char **)subpaths);
        }

      return op;
    }

  op = flatpak_transaction_operation_new (remote, ref, subpaths, commit, install, update);
  g_hash_table_insert (self->refs, g_strdup (ref), op);
  self->ops = g_list_prepend (self->ops, op);

  return op;
}

static char *
ask_for_remote (const char **remotes)
{
  int n_remotes = g_strv_length ((char **)remotes);
  int chosen = 0;
  int i;

  if (n_remotes == 1)
    {
      if (flatpak_yes_no_prompt (_("Found in remote %s, do you want to install it?"), remotes[0]))
        chosen = 1;
    }
  else
    {
      g_print (_("Found in several remotes:\n"));
      for (i = 0; remotes[i] != NULL; i++)
        {
          g_print ("%d) %s\n", i + 1, remotes[i]);
        }
      chosen = flatpak_number_prompt (0, n_remotes, _("Which do you want to install (0 to abort)?"));
    }

  if (chosen == 0)
    return NULL;

  return g_strdup (remotes[chosen-1]);
}

static gboolean
add_related (FlatpakTransaction *self,
             const char *remote,
             const char *ref,
             GError **error)
{
  g_autoptr(GPtrArray) related = NULL;
  g_autoptr(GError) local_error = NULL;
  int i;

  if (!self->add_related)
    return TRUE;

  if (self->no_pull)
    related = flatpak_dir_find_local_related (self->dir, ref, remote, NULL, &local_error);
  else
    related = flatpak_dir_find_remote_related (self->dir, ref, remote, NULL, &local_error);
  if (related == NULL)
    {
      g_printerr (_("Warning: Problem looking for related refs: %s\n"), local_error->message);
      g_clear_error (&local_error);
    }
  else
    {
      for (i = 0; i < related->len; i++)
        {
          FlatpakRelated *rel = g_ptr_array_index (related, i);
          FlatpakTransactionOp *op;

          if (!rel->download)
            continue;

          op = flatpak_transaction_add_op (self, remote, rel->ref,
                                           (const char **)rel->subpaths,
                                           NULL, TRUE, TRUE);
          op->non_fatal = TRUE;
        }
    }

  return TRUE;
}

static gboolean
add_deps (FlatpakTransaction *self,
          const char *remote,
          const char *ref,
          GError **error)
{
  g_autofree char *runtime_ref = NULL;
  g_autofree char *full_runtime_ref = NULL;
  g_autofree char *runtime_remote = NULL;
  const char *pref;

  runtime_ref = transaction_fetch_runtime_ref (self->dir, remote, ref);
  if (runtime_ref == NULL)
    return TRUE;

  pref = strchr (ref, '/') + 1;

  full_runtime_ref = g_strconcat ("runtime/", runtime_ref, NULL);

  if (!flatpak_transaction_contains_ref (self, full_runtime_ref))
    {
      if (!ref_is_installed (self->dir, full_runtime_ref))
        {
          g_auto(GStrv) remotes = NULL;

          g_print (_("Required runtime for %s (%s) is not installed, searching...\n"),
                   pref, runtime_ref);

          remotes = flatpak_dir_search_for_dependency (self->dir, full_runtime_ref, NULL, NULL);
          if (remotes == NULL || *remotes == NULL)
            {
              g_print (_("The required runtime %s was not found in a configured remote.\n"),
                       runtime_ref);
            }
          else
            {
              runtime_remote = ask_for_remote ((const char **)remotes);
            }

          if (runtime_remote == NULL)
            return flatpak_fail (error,
                                 "The Application %s requires the runtime %s which is not installed",
                                 pref, runtime_ref);

          flatpak_transaction_add_op (self, runtime_remote, full_runtime_ref, NULL, NULL, TRUE, TRUE);
        }
      else
        {
          /* Update if in same dir */
          if (dir_ref_is_installed (self->dir, full_runtime_ref, &runtime_remote))
            {
              flatpak_transaction_add_op (self, runtime_remote, full_runtime_ref, NULL, NULL, FALSE, TRUE);
            }
        }
    }

  if (runtime_remote != NULL &&
      !add_related (self, runtime_remote, full_runtime_ref, error))
    return FALSE;

  return TRUE;
}

static gboolean
flatpak_transaction_add_ref (FlatpakTransaction *self,
                             const char *remote,
                             const char *ref,
                             const char **subpaths,
                             const char *commit,
                             gboolean is_update,
                             GError **error)
{
  g_autofree char *origin = NULL;
  const char *pref;

  pref = strchr (ref, '/') + 1;

  if (is_update)
    {
      if (!dir_ref_is_installed (self->dir, ref, &origin))
        {
          g_set_error (error, FLATPAK_ERROR, FLATPAK_ERROR_NOT_INSTALLED,
                       _("%s not installed"), pref);
          return FALSE;
        }

      if (flatpak_dir_get_remote_disabled (self->dir, origin))
        {
          g_debug (_("Remote %s disabled, ignoring %s update"), origin, pref);
          return TRUE;
        }
      remote = origin;
    }
  else
    {
      g_assert (remote != NULL);
      if (dir_ref_is_installed (self->dir, ref, NULL))
        {
          g_set_error (error, FLATPAK_ERROR, FLATPAK_ERROR_ALREADY_INSTALLED,
                       _("%s already installed"), pref);
          return FALSE;
        }
    }

  if (self->add_deps)
    add_deps (self, remote, ref, error);

  flatpak_transaction_add_op (self, remote, ref, subpaths, commit, !is_update, is_update);

  if (!add_related (self, remote, ref, error))
    return FALSE;

  return TRUE;
}

gboolean
flatpak_transaction_add_install (FlatpakTransaction *self,
                                 const char *remote,
                                 const char *ref,
                                 const char **subpaths,
                                 GError **error)
{
  const char *all_paths[] = { NULL };

  /* If we install with no special args pull all subpaths */
  if (subpaths == NULL)
    subpaths = all_paths;

  return flatpak_transaction_add_ref (self, remote, ref, subpaths, NULL, FALSE, error);
}

gboolean
flatpak_transaction_add_install_oci (FlatpakTransaction  *self,
                                     const char         *uri,
                                     const char         *tag,
                                     GError             **error)
{
  GHashTable *annotations;
  g_autofree char *ref = NULL;
  g_autofree char *checksum = NULL;
  g_autoptr(FlatpakOciManifest) manifest = NULL;
  g_autoptr(FlatpakOciRegistry) registry = NULL;
  const char *all_paths[] = { NULL };
  g_autofree char *remote = NULL;
  g_autofree char *title = NULL;
  g_autofree char **parts = NULL;
  g_autofree char *id = NULL;

  registry = flatpak_oci_registry_new (uri, FALSE, -1, NULL, error);
  if (registry == NULL)
    return FALSE;

  manifest = flatpak_oci_registry_chose_image (registry, tag, NULL,
                                               NULL, error);
  if (manifest == NULL)
    return FALSE;

  /* TODO: Extract runtime dependencies and related refs */
  annotations = flatpak_oci_manifest_get_annotations (manifest);
  if (annotations)
    flatpak_oci_parse_commit_annotations (annotations, NULL,
                                          NULL, NULL,
                                          &ref, &checksum, NULL,
                                          NULL);

  if (ref == NULL)
    return flatpak_fail (error, _("OCI image is not a flatpak (missing ref)"));

  parts = flatpak_decompose_ref (ref, error);
  if (parts == NULL)
    return FALSE;

  title = g_strdup_printf ("OCI remote for %s", parts[1]);

  id = g_strdup_printf ("oci-%s", parts[1]);

  remote = flatpak_dir_create_origin_remote (self->dir, NULL,
                                            id, title,
                                             ref, uri, tag, NULL,
                                             NULL, error);
  if (remote == NULL)
    return FALSE;

  if (!flatpak_dir_recreate_repo (self->dir, NULL, error))
    return FALSE;

  g_debug ("Added OCI origin remote %s", remote);

  return flatpak_transaction_add_ref (self, remote, ref, all_paths, checksum, FALSE, error);
}

gboolean
flatpak_transaction_add_update (FlatpakTransaction *self,
                                const char *ref,
                                const char **subpaths,
                                const char *commit,
                                GError **error)
{
  return flatpak_transaction_add_ref (self, NULL, ref, subpaths, commit, TRUE, error);
}

gboolean
flatpak_transaction_run (FlatpakTransaction *self,
                         gboolean stop_on_first_error,
                         GCancellable *cancellable,
                         GError **error)
{
  GList *l;
  gboolean succeeded = TRUE;

  self->ops = g_list_reverse (self->ops);

  for (l = self->ops; l != NULL; l = l->next)
    {
      FlatpakTransactionOp *op = l->data;
      g_autoptr(GError) local_error = NULL;
      gboolean res;
      const char *pref;
      const char *opname;

      if (op->install && op->update)
        {
          if (dir_ref_is_installed (self->dir, op->ref, NULL))
            op->install = FALSE;
          else
            op->update = FALSE;
        }

      pref = strchr (op->ref, '/') + 1;

      if (op->install)
        {
          opname = _("install");
          g_print (_("Installing: %s from %s\n"), pref, op->remote);
          res = flatpak_dir_install (self->dir,
                                     self->no_pull,
                                     self->no_deploy,
                                     op->ref, op->remote,
                                     (const char **)op->subpaths,
                                     NULL,
                                     cancellable, &local_error);
        }
      else /* update */
        {
          opname = _("update");
          g_print (_("Updating: %s from %s\n"), pref, op->remote);
          res = flatpak_dir_update (self->dir,
                                    self->no_pull,
                                    self->no_deploy,
                                    op->ref, op->remote, op->commit,
                                    (const char **)op->subpaths,
                                    NULL,
                                    cancellable, &local_error);

          if (res)
            {
              g_autoptr(GVariant) deploy_data = NULL;
              g_autofree char *commit = NULL;
              deploy_data = flatpak_dir_get_deploy_data (self->dir, op->ref, NULL, NULL);
              commit = g_strndup (flatpak_deploy_data_get_commit (deploy_data), 12);
              g_print (_("Now at %s.\n"), commit);
            }

          /* Handle noop-updates */
          if (!res && g_error_matches (local_error, FLATPAK_ERROR, FLATPAK_ERROR_ALREADY_INSTALLED))
            {
              g_print (_("No updates.\n"));
              res = TRUE;
              g_clear_error (&local_error);
            }
        }

      if (!res)
        {
          if (op->non_fatal)
            {
              g_printerr (_("Warning: Failed to %s %s: %s\n"),
                          opname, pref, local_error->message);
            }
          else if (!stop_on_first_error)
            {
              g_printerr (_("Error: Failed to %s %s: %s\n"),
                          opname, pref, local_error->message);
              if (succeeded)
                {
                  succeeded = FALSE;
                  flatpak_fail (error, _("One or more operations failed"));
                }
            }
          else
            {
              g_propagate_error (error, g_steal_pointer (&local_error));
              return FALSE;
            }
        }
    }

  return succeeded;
}
