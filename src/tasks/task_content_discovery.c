/*  RetroArch - A frontend for libretro.
 *
 *  Rule-driven content discovery for appliance-style frontends.
 *  The scanner runs on RetroArch's task queue, writes playlists through the
 *  normal playlist API and refreshes the active menu when it completes.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <file/config_file.h>
#include <file/file_path.h>
#include <lists/dir_list.h>
#include <lists/string_list.h>
#include <playlists/label_sanitization.h>
#include <streams/file_stream.h>
#include <string/stdstring.h>

#include "tasks_internal.h"

#include "../configuration.h"
#include "../defaults.h"
#include "../file_path_special.h"
#include "../frontend/frontend_driver.h"
#include "../verbosity.h"

#ifdef HAVE_MENU
#include "../menu/menu_driver.h"
#endif

#define CONTENT_DISCOVERY_MAX_ROOTS 4
#define CONTENT_DISCOVERY_MAX_RULES 16

enum content_discovery_label_mode
{
   CONTENT_DISCOVERY_LABEL_FILENAME = 0,
   CONTENT_DISCOVERY_LABEL_CLEAN,
   CONTENT_DISCOVERY_LABEL_KEEP_DISC
};

enum content_discovery_status
{
   CONTENT_DISCOVERY_BEGIN_RULE = 0,
   CONTENT_DISCOVERY_SCAN_ROOT,
   CONTENT_DISCOVERY_WRITE_PLAYLIST,
   CONTENT_DISCOVERY_END
};

typedef struct content_discovery_rule
{
   char directory[NAME_MAX_LENGTH];
   char playlist_name[NAME_MAX_LENGTH];
   char extensions[NAME_MAX_LENGTH];
   char core_path[PATH_MAX_LENGTH];
   char core_name[NAME_MAX_LENGTH];
   char db_name[NAME_MAX_LENGTH];
   enum content_discovery_label_mode label_mode;
   bool recursive;
} content_discovery_rule_t;

typedef struct content_discovery_handle
{
   struct string_list *content;
   struct string_list *rule_directories;
   struct string_list *current_content;
   struct string_list *active_directories;
   struct string_list *generated_playlists;
   char roots[CONTENT_DISCOVERY_MAX_ROOTS][DIR_MAX_LENGTH];
   char playlist_directory[DIR_MAX_LENGTH];
   content_discovery_rule_t rules[CONTENT_DISCOVERY_MAX_RULES];
   size_t root_count;
   size_t rule_count;
   size_t root_index;
   size_t rule_index;
   size_t found;
   size_t added;
   size_t removed;
   size_t updated;
   size_t playlists_changed;
   size_t stale_entries_removed;
   size_t errors;
   bool rule_scan_failed;
   bool completed;
   enum content_discovery_status status;
} content_discovery_handle_t;

static void content_discovery_notify(const char *msg,
      unsigned priority, unsigned duration,
      enum message_queue_category category)
{
   if (!msg || !*msg)
      return;

   runloop_msg_queue_push(msg, strlen(msg), priority, duration, true, NULL,
         MESSAGE_QUEUE_ICON_DEFAULT, category);
}

static bool content_discovery_name_is_safe(const char *name)
{
   if (!name || !*name)
      return false;

   return !strstr(name, "..") && !strchr(name, '/') && !strchr(name, '\\');
}

static bool content_discovery_directory_is_safe(const char *directory)
{
   if (!directory || !*directory || path_is_absolute(directory))
      return false;

   return !strstr(directory, "..");
}

static void content_discovery_set_task_title(retro_task_t *task,
      const char *playlist_name, size_t root_index, size_t root_count)
{
   char title[256];

   if (!task)
      return;

   if (playlist_name && *playlist_name && root_count > 0)
      snprintf(title, sizeof(title), "Scanning %s (%u/%u)...",
            playlist_name, (unsigned)(root_index + 1), (unsigned)root_count);
   else
      strlcpy(title, "Scanning games...", sizeof(title));

   task_free_title(task);
   task_set_title(task, strdup(title));
}

static void content_discovery_make_label(
      const content_discovery_rule_t *rule, const char *path,
      char *label, size_t len)
{
   if (!rule || !path || !label || len < 1)
      return;

   fill_pathname(label, path_basename(path), "", len);

   switch (rule->label_mode)
   {
      case CONTENT_DISCOVERY_LABEL_CLEAN:
         label_remove_parens_and_brackets(label);
         break;
      case CONTENT_DISCOVERY_LABEL_KEEP_DISC:
         /* Removes region/revision tags, but retains (Disc 1), (Disc 2), ... */
         label_keep_disc(label);
         break;
      case CONTENT_DISCOVERY_LABEL_FILENAME:
      default:
         break;
   }
}

static bool content_discovery_string_equal(const char *a, const char *b)
{
   if (!a || !*a)
      return !b || !*b;
   if (!b || !*b)
      return false;
   return string_is_equal(a, b);
}

static bool content_discovery_list_has_path(
      const struct string_list *list, const char *path)
{
   return list && path && *path && string_list_find_elem(list, path) > 0;
}

static bool content_discovery_list_append_unique(
      struct string_list *list, const char *value)
{
   union string_list_elem_attr attr;

   if (!list || !value || !*value)
      return false;
   if (content_discovery_list_has_path(list, value))
      return true;

   attr.i = 0;
   return string_list_append(list, value, attr);
}

static bool content_discovery_path_is_in_active_directory(
      const content_discovery_handle_t *discovery, const char *path)
{
   size_t i;

   if (!discovery || !discovery->active_directories || !path || !*path)
      return false;

   for (i = 0; i < discovery->active_directories->size; i++)
   {
      const char *directory = discovery->active_directories->elems[i].data;
      size_t directory_len  = strlen(directory);

      if (   directory_len > 0
          && string_starts_with_case_insensitive(path, directory)
          && (PATH_CHAR_IS_SLASH(directory[directory_len - 1])
              || path[directory_len] == '\0'
              || PATH_CHAR_IS_SLASH(path[directory_len])))
         return true;
   }

   return false;
}

static size_t content_discovery_clean_reference_playlist(
      playlist_t *playlist, const content_discovery_handle_t *discovery)
{
   size_t i;
   size_t old_size;

   if (!playlist || !discovery || !discovery->current_content)
      return 0;

   old_size = playlist_size(playlist);
   for (i = old_size; i > 0; i--)
   {
      const struct playlist_entry *entry = NULL;

      playlist_get_index(playlist, i - 1, &entry);
      if (   entry && entry->path && *entry->path
          && content_discovery_path_is_in_active_directory(
                discovery, entry->path)
          && !content_discovery_list_has_path(
                discovery->current_content, entry->path))
         playlist_delete_index(playlist, i - 1);
   }

   if (playlist_size(playlist) != old_size)
      playlist_write_file(playlist);

   return old_size - playlist_size(playlist);
}

static bool content_discovery_playlist_is_current(playlist_t *playlist,
      const struct string_list *content,
      const content_discovery_rule_t *rule,
      size_t *added, size_t *removed, size_t *updated)
{
   size_t i;
   size_t old_size;
   bool current = true;

   if (!playlist || !content || !rule)
      return false;

   old_size = playlist_size(playlist);

   for (i = 0; i < content->size; i++)
   {
      char label[PATH_MAX_LENGTH];
      size_t j;
      const struct playlist_entry *matched_entry = NULL;

      content_discovery_make_label(rule, content->elems[i].data,
            label, sizeof(label));

      for (j = 0; j < old_size; j++)
      {
         const struct playlist_entry *entry = NULL;
         playlist_get_index(playlist, j, &entry);
         if (entry && content_discovery_string_equal(
                  entry->path, content->elems[i].data))
         {
            matched_entry = entry;
            break;
         }
      }

      if (!matched_entry)
      {
         if (added)
            (*added)++;
         current = false;
      }
      else if (   !content_discovery_string_equal(matched_entry->label, label)
               || !content_discovery_string_equal(
                     matched_entry->core_path, rule->core_path)
               || !content_discovery_string_equal(
                     matched_entry->core_name, rule->core_name)
               || !content_discovery_string_equal(
                     matched_entry->db_name, rule->db_name))
      {
         RARCH_LOG("[Content Discovery] Entry metadata changed: %s "
               "(label=\"%s\"/\"%s\", core=\"%s\"/\"%s\", "
               "core_name=\"%s\"/\"%s\", db=\"%s\"/\"%s\")\n",
               content->elems[i].data,
               matched_entry->label ? matched_entry->label : "", label,
               matched_entry->core_path ? matched_entry->core_path : "",
               rule->core_path,
               matched_entry->core_name ? matched_entry->core_name : "",
               rule->core_name,
               matched_entry->db_name ? matched_entry->db_name : "",
               rule->db_name);
         if (updated)
            (*updated)++;
         current = false;
      }
   }

   for (i = 0; i < old_size; i++)
   {
      const struct playlist_entry *entry = NULL;
      playlist_get_index(playlist, i, &entry);
      if (entry && !content_discovery_list_has_path(content, entry->path))
      {
         if (removed)
            (*removed)++;
         current = false;
      }
   }

   if (old_size != content->size)
      current = false;

   if (!content_discovery_string_equal(
            playlist_get_default_core_path(playlist), rule->core_path)
       || !content_discovery_string_equal(
            playlist_get_default_core_name(playlist), rule->core_name)
       || playlist_get_label_display_mode(playlist)
            != LABEL_DISPLAY_MODE_DEFAULT
       || playlist_get_sort_mode(playlist) != PLAYLIST_SORT_MODE_ALPHABETICAL)
   {
      RARCH_LOG("[Content Discovery] Playlist metadata changed: "
            "core=\"%s\"/\"%s\", core_name=\"%s\"/\"%s\", "
            "label_mode=%u/%u, sort_mode=%u/%u\n",
            playlist_get_default_core_path(playlist)
                  ? playlist_get_default_core_path(playlist) : "",
            rule->core_path,
            playlist_get_default_core_name(playlist)
                  ? playlist_get_default_core_name(playlist) : "",
            rule->core_name,
            (unsigned)playlist_get_label_display_mode(playlist),
            (unsigned)LABEL_DISPLAY_MODE_DEFAULT,
            (unsigned)playlist_get_sort_mode(playlist),
            (unsigned)PLAYLIST_SORT_MODE_ALPHABETICAL);
      current = false;
   }

   return current;
}

static bool content_discovery_write_rule(content_discovery_handle_t *discovery,
      const content_discovery_rule_t *rule)
{
   char playlist_file[NAME_MAX_LENGTH];
   char playlist_path[PATH_MAX_LENGTH];
   playlist_config_t playlist_config;
   playlist_t *playlist = NULL;
   size_t old_size;
   size_t i;
   bool playlist_existed;
   bool playlist_current;
   union string_list_elem_attr attr;

   if (!discovery || !rule || !discovery->content)
      return false;

   snprintf(playlist_file, sizeof(playlist_file), "%s.lpl",
         rule->playlist_name);
   fill_pathname_join_special(playlist_path,
         discovery->playlist_directory, playlist_file,
         sizeof(playlist_path));

   attr.i = 0;
   if (!string_list_find_elem(discovery->generated_playlists, playlist_path))
      string_list_append(discovery->generated_playlists, playlist_path, attr);

   memset(&playlist_config, 0, sizeof(playlist_config));
   playlist_config.capacity = COLLECTION_SIZE;
   playlist_config.old_format = false;
   playlist_config.compress = false;
   playlist_config_set_path(&playlist_config, playlist_path);

   playlist_existed = path_is_valid(playlist_path);
   if (!(playlist = playlist_init(&playlist_config)))
   {
      RARCH_ERR("[Content Discovery] Failed to open playlist: %s\n",
            playlist_path);
      return false;
   }

   old_size = playlist_size(playlist);

   if (discovery->content->size < 1)
   {
      discovery->removed += old_size;
      playlist_free(playlist);

      if (playlist_existed)
      {
         if (filestream_delete(playlist_path) == 0)
         {
            discovery->playlists_changed++;
            RARCH_LOG("[Content Discovery] Removed empty playlist: %s\n",
                  playlist_path);
            return true;
         }

         RARCH_ERR("[Content Discovery] Failed to remove empty playlist: %s\n",
               playlist_path);
         return false;
      }

      return true;
   }

   playlist_current = content_discovery_playlist_is_current(playlist,
         discovery->content, rule, &discovery->added,
         &discovery->removed, &discovery->updated);

   if (playlist_current)
   {
      playlist_free(playlist);
      RARCH_LOG("[Content Discovery] Playlist is current: %s (%u entries)\n",
            playlist_path, (unsigned)discovery->content->size);
      return true;
   }

   while (playlist_size(playlist) > 0)
      playlist_delete_index(playlist, playlist_size(playlist) - 1);

   playlist_set_default_core_path(playlist, rule->core_path);
   playlist_set_default_core_name(playlist, rule->core_name);
   playlist_set_label_display_mode(playlist, LABEL_DISPLAY_MODE_DEFAULT);
   playlist_set_sort_mode(playlist, PLAYLIST_SORT_MODE_ALPHABETICAL);

   for (i = 0; i < discovery->content->size; i++)
   {
      char label[PATH_MAX_LENGTH];
      struct playlist_entry entry;

      memset(&entry, 0, sizeof(entry));
      content_discovery_make_label(rule, discovery->content->elems[i].data,
            label, sizeof(label));

      entry.path      = discovery->content->elems[i].data;
      entry.label     = label;
      entry.core_path = (char*)rule->core_path;
      entry.core_name = (char*)rule->core_name;
      entry.db_name   = (char*)rule->db_name;
      entry.crc32     = (char*)FILE_PATH_DETECT;

      if (!playlist_push(playlist, &entry))
      {
         RARCH_ERR("[Content Discovery] Failed to add content: %s\n",
               entry.path);
         playlist_free(playlist);
         return false;
      }
   }

   playlist_qsort(playlist);
   playlist_write_file(playlist);
   playlist_free(playlist);

   playlist = playlist_init(&playlist_config);
   if (   !path_is_valid(playlist_path)
       || !playlist
       || !content_discovery_playlist_is_current(playlist,
             discovery->content, rule, NULL, NULL, NULL))
   {
      if (playlist)
         playlist_free(playlist);
      RARCH_ERR("[Content Discovery] Playlist was not written: %s\n",
            playlist_path);
      return false;
   }
   playlist_free(playlist);

   discovery->playlists_changed++;
   RARCH_LOG("[Content Discovery] Wrote playlist: %s (%u entries)\n",
         playlist_path, (unsigned)discovery->content->size);
   return true;
}

static void content_discovery_handle_free(content_discovery_handle_t *discovery)
{
   if (!discovery)
      return;

   if (discovery->content)
      string_list_free(discovery->content);
   if (discovery->rule_directories)
      string_list_free(discovery->rule_directories);
   if (discovery->current_content)
      string_list_free(discovery->current_content);
   if (discovery->active_directories)
      string_list_free(discovery->active_directories);
   if (discovery->generated_playlists)
      string_list_free(discovery->generated_playlists);
   free(discovery);
}

static void content_discovery_task_free(retro_task_t *task)
{
   if (task)
      content_discovery_handle_free(
            (content_discovery_handle_t*)task->state);
}

static void content_discovery_task_handler(retro_task_t *task)
{
   content_discovery_handle_t *discovery = NULL;
   content_discovery_rule_t *rule = NULL;
   uint8_t flags;

   if (!task || !(discovery = (content_discovery_handle_t*)task->state))
      goto task_finished;

   flags = task_get_flags(task);
   if ((flags & RETRO_TASK_FLG_CANCELLED) > 0)
      goto task_finished;

   if (discovery->rule_index >= discovery->rule_count)
      discovery->status = CONTENT_DISCOVERY_END;

   if (discovery->status != CONTENT_DISCOVERY_END)
      rule = &discovery->rules[discovery->rule_index];

   switch (discovery->status)
   {
      case CONTENT_DISCOVERY_BEGIN_RULE:
         if (discovery->content)
            string_list_free(discovery->content);
         discovery->content = string_list_new();
         if (discovery->rule_directories)
            string_list_free(discovery->rule_directories);
         discovery->rule_directories = string_list_new();
         if (!discovery->content || !discovery->rule_directories)
         {
            discovery->errors++;
            goto task_finished;
         }

         discovery->root_index = 0;
         discovery->rule_scan_failed = false;
         discovery->status = CONTENT_DISCOVERY_SCAN_ROOT;
         content_discovery_set_task_title(task, rule->playlist_name,
               discovery->root_index, discovery->root_count);
         return;

      case CONTENT_DISCOVERY_SCAN_ROOT:
         if (discovery->root_index < discovery->root_count)
         {
            char content_dir[DIR_MAX_LENGTH];
            size_t completed_steps;
            size_t total_steps = discovery->rule_count *
                  (discovery->root_count + 1);

            fill_pathname_join_special(content_dir,
                  discovery->roots[discovery->root_index], rule->directory,
                  sizeof(content_dir));

            if (path_is_directory(content_dir))
            {
               if (!dir_list_append(discovery->content, content_dir,
                        rule->extensions, false, false, false,
                        rule->recursive))
               {
                  discovery->errors++;
                  discovery->rule_scan_failed = true;
                  RARCH_WARN("[Content Discovery] Cannot scan: %s\n",
                        content_dir);
               }
               else
               {
                  if (!content_discovery_list_append_unique(
                        discovery->rule_directories, content_dir))
                  {
                     discovery->errors++;
                     discovery->rule_scan_failed = true;
                  }
                  RARCH_LOG("[Content Discovery] Scanned: %s\n", content_dir);
               }
            }
            else
               RARCH_LOG("[Content Discovery] Folder not present: %s\n",
                     content_dir);

            discovery->root_index++;
            completed_steps = discovery->rule_index *
                  (discovery->root_count + 1) + discovery->root_index;
            if (total_steps > 0)
               task_set_progress(task,
                     (int8_t)((completed_steps * 100) / total_steps));

            if (discovery->root_index < discovery->root_count)
               content_discovery_set_task_title(task, rule->playlist_name,
                     discovery->root_index, discovery->root_count);
            else
            {
               dir_list_sort(discovery->content, false);
               discovery->status = CONTENT_DISCOVERY_WRITE_PLAYLIST;
            }
         }
         else
            discovery->status = CONTENT_DISCOVERY_WRITE_PLAYLIST;
         return;

      case CONTENT_DISCOVERY_WRITE_PLAYLIST:
         if (!discovery->rule_scan_failed)
         {
            size_t i;

            for (i = 0; i < discovery->content->size; i++)
               if (!content_discovery_list_append_unique(
                     discovery->current_content,
                     discovery->content->elems[i].data))
               {
                  discovery->errors++;
                  discovery->rule_scan_failed = true;
                  break;
               }

            if (!discovery->rule_scan_failed)
               for (i = 0; i < discovery->rule_directories->size; i++)
                  if (!content_discovery_list_append_unique(
                        discovery->active_directories,
                        discovery->rule_directories->elems[i].data))
                  {
                     discovery->errors++;
                     discovery->rule_scan_failed = true;
                     break;
                  }
         }
         discovery->found += discovery->content->size;

         if (discovery->rule_scan_failed)
            RARCH_WARN("[Content Discovery] Keeping existing playlist after "
                  "an incomplete scan: %s\n", rule->playlist_name);
         else if (!path_is_valid(rule->core_path))
         {
            discovery->errors++;
            RARCH_ERR("[Content Discovery] Core not found: %s\n",
                  rule->core_path);
         }
         else if (!content_discovery_write_rule(discovery, rule))
            discovery->errors++;

         string_list_free(discovery->content);
         discovery->content = NULL;
         discovery->rule_index++;
         discovery->status = discovery->rule_index < discovery->rule_count
               ? CONTENT_DISCOVERY_BEGIN_RULE : CONTENT_DISCOVERY_END;
         return;

      case CONTENT_DISCOVERY_END:
      default:
         task_set_progress(task, 100);
         discovery->completed = true;
         goto task_finished;
   }

task_finished:
   if (task)
      task_set_flags(task, RETRO_TASK_FLG_FINISHED, true);
}

static void content_discovery_task_callback(retro_task_t *task,
      void *task_data, void *user_data, const char *err)
{
   char message[256];
   content_discovery_handle_t *discovery = task
         ? (content_discovery_handle_t*)task->state : NULL;

   (void)task_data;
   (void)user_data;
   (void)err;

   if (!discovery)
      return;

   if (discovery->completed)
   {
      discovery->stale_entries_removed +=
            content_discovery_clean_reference_playlist(
                  g_defaults.content_history, discovery);
      discovery->stale_entries_removed +=
            content_discovery_clean_reference_playlist(
                  g_defaults.content_favorites, discovery);

      if (discovery->stale_entries_removed > 0)
         RARCH_LOG("[Content Discovery] Removed %u stale History/Favorites "
               "entr%s.\n", (unsigned)discovery->stale_entries_removed,
               discovery->stale_entries_removed == 1 ? "y" : "ies");
   }

#ifdef HAVE_MENU
   if (   discovery->playlists_changed > 0
       || discovery->stale_entries_removed > 0)
   {
      playlist_t *cached_playlist = playlist_get_cached();
      struct menu_state *menu_st = menu_state_get_ptr();

      if (cached_playlist && discovery->generated_playlists)
      {
         const char *cached_path = playlist_get_conf_path(cached_playlist);
         if (content_discovery_list_has_path(
                  discovery->generated_playlists, cached_path))
         {
            playlist_config_t cached_config;
            if (playlist_config_copy(
                  playlist_get_config(cached_playlist), &cached_config))
            {
               playlist_free_cached();
               if (path_is_valid(cached_config.path))
                  playlist_init_cached(&cached_config);
            }
         }
      }

      if (menu_st && menu_st->driver_ctx && menu_st->driver_ctx->environ_cb)
      {
         menu_st->flags |= MENU_ST_FLAG_ENTRIES_NEED_REFRESH;
         menu_st->driver_ctx->environ_cb(MENU_ENVIRON_RESET_HORIZONTAL_LIST,
               NULL, menu_st->userdata);
      }
   }
#endif

   if (discovery->errors > 0)
      snprintf(message, sizeof(message),
            "Game scan finished: %u found, %u error%s.",
            (unsigned)discovery->found, (unsigned)discovery->errors,
            discovery->errors == 1 ? "" : "s");
   else if (   discovery->playlists_changed > 0
            || discovery->stale_entries_removed > 0)
   {
      if (discovery->stale_entries_removed > 0)
         snprintf(message, sizeof(message),
               "Games updated: %u found; cleared %u stale "
               "History/Favorite entr%s.",
               (unsigned)discovery->found,
               (unsigned)discovery->stale_entries_removed,
               discovery->stale_entries_removed == 1 ? "y" : "ies");
      else
         snprintf(message, sizeof(message),
               "Games updated: %u found (+%u, -%u, %u changed).",
               (unsigned)discovery->found, (unsigned)discovery->added,
               (unsigned)discovery->removed, (unsigned)discovery->updated);
   }
   else
      snprintf(message, sizeof(message),
            "Game library is up to date: %u game%s.",
            (unsigned)discovery->found, discovery->found == 1 ? "" : "s");

   content_discovery_notify(message, discovery->errors > 0 ? 2 : 1, 240,
         discovery->errors > 0
               ? MESSAGE_QUEUE_CATEGORY_ERROR
               : (   discovery->playlists_changed > 0
                  || discovery->stale_entries_removed > 0)
                     ? MESSAGE_QUEUE_CATEGORY_SUCCESS
                     : MESSAGE_QUEUE_CATEGORY_INFO);
}

static bool content_discovery_task_finder(retro_task_t *task, void *user_data)
{
   (void)user_data;
   return task && task->handler == content_discovery_task_handler;
}

static bool content_discovery_parse_config(content_discovery_handle_t *discovery,
      const char *rules_path, const char *core_directory)
{
   config_file_t *config = NULL;
   char core_extension[32];
   size_t i;

   if (!discovery || !rules_path || !*rules_path ||
       !core_directory || !*core_directory)
      return false;

   if (!(config = config_file_new_from_path_to_string(rules_path)))
      return false;

   core_extension[0] = '\0';
   frontend_driver_get_core_extension(core_extension,
         sizeof(core_extension));

   for (i = 0; i < CONTENT_DISCOVERY_MAX_ROOTS; i++)
   {
      char key[32];
      char value[DIR_MAX_LENGTH];

      snprintf(key, sizeof(key), "root%u", (unsigned)i);
      if (!config_get_array(config, key, value, sizeof(value)))
         break;

      if (!*value)
         continue;

      if (path_is_absolute(value))
         strlcpy(discovery->roots[discovery->root_count], value,
               sizeof(discovery->roots[discovery->root_count]));
      else
         fill_pathname_resolve_relative(
               discovery->roots[discovery->root_count], rules_path, value,
               sizeof(discovery->roots[discovery->root_count]));

      discovery->root_count++;
   }

   for (i = 0; i < CONTENT_DISCOVERY_MAX_RULES; i++)
   {
      char key[64];
      char core_file[NAME_MAX_LENGTH];
      char core_id[NAME_MAX_LENGTH];
      char label_mode[64];
      content_discovery_rule_t *rule = &discovery->rules[discovery->rule_count];

      snprintf(key, sizeof(key), "rule%u_directory", (unsigned)i);
      if (!config_get_array(config, key, rule->directory,
               sizeof(rule->directory)))
         break;

      snprintf(key, sizeof(key), "rule%u_playlist", (unsigned)i);
      if (!config_get_array(config, key, rule->playlist_name,
               sizeof(rule->playlist_name)))
         goto invalid_rule;

      snprintf(key, sizeof(key), "rule%u_extensions", (unsigned)i);
      if (!config_get_array(config, key, rule->extensions,
               sizeof(rule->extensions)))
         goto invalid_rule;

      snprintf(key, sizeof(key), "rule%u_core", (unsigned)i);
      if (!config_get_array(config, key, core_id, sizeof(core_id)))
         goto invalid_rule;

      snprintf(key, sizeof(key), "rule%u_core_name", (unsigned)i);
      if (!config_get_array(config, key, rule->core_name,
               sizeof(rule->core_name)))
         goto invalid_rule;

      if (   !content_discovery_directory_is_safe(rule->directory)
          || !content_discovery_name_is_safe(rule->playlist_name)
          || !content_discovery_name_is_safe(core_id)
          || !*rule->extensions || !*rule->core_name)
         goto invalid_rule;

      if (*path_get_extension(core_id))
         strlcpy(core_file, core_id, sizeof(core_file));
      else
         snprintf(core_file, sizeof(core_file), "%s.%s",
               core_id, core_extension);

      fill_pathname_join_special(rule->core_path, core_directory,
            core_file, sizeof(rule->core_path));
      /* Playlist writes canonical core paths. Match that representation here
       * so an unchanged library does not appear modified on every startup. */
      path_resolve_realpath(rule->core_path, sizeof(rule->core_path), true);
      snprintf(rule->db_name, sizeof(rule->db_name), "%s.lpl",
            rule->playlist_name);

      rule->recursive = true;
      snprintf(key, sizeof(key), "rule%u_recursive", (unsigned)i);
      config_get_bool(config, key, &rule->recursive);

      rule->label_mode = CONTENT_DISCOVERY_LABEL_KEEP_DISC;
      label_mode[0] = '\0';
      snprintf(key, sizeof(key), "rule%u_label_mode", (unsigned)i);
      if (config_get_array(config, key, label_mode, sizeof(label_mode)))
      {
         if (string_is_equal(label_mode, "filename"))
            rule->label_mode = CONTENT_DISCOVERY_LABEL_FILENAME;
         else if (string_is_equal(label_mode, "clean"))
            rule->label_mode = CONTENT_DISCOVERY_LABEL_CLEAN;
         else if (!string_is_equal(label_mode, "keep_disc"))
            goto invalid_rule;
      }

      discovery->rule_count++;
      continue;

invalid_rule:
      RARCH_ERR("[Content Discovery] Invalid rule%u in %s\n",
            (unsigned)i, rules_path);
      config_file_free(config);
      return false;
   }

   config_file_free(config);
   return discovery->root_count > 0 && discovery->rule_count > 0;
}

bool task_push_content_discovery(void)
{
   const char *rules_path = getenv("RA_CONTENT_RULES");
   settings_t *settings = config_get_ptr();
   content_discovery_handle_t *discovery = NULL;
   retro_task_t *task = NULL;
   task_finder_data_t find_data;

   if (!rules_path || !*rules_path)
      return false;

   if (!settings || !*settings->paths.directory_playlist ||
       !*settings->paths.directory_libretro)
      goto error;

   find_data.func = content_discovery_task_finder;
   find_data.userdata = NULL;
   if (task_queue_find(&find_data))
      return false;

   if (!(task = task_init()))
      goto error;
   if (!(discovery = (content_discovery_handle_t*)calloc(1,
            sizeof(content_discovery_handle_t))))
      goto error;
   if (!(discovery->current_content = string_list_new()))
      goto error;
   if (!(discovery->active_directories = string_list_new()))
      goto error;
   if (!(discovery->generated_playlists = string_list_new()))
      goto error;

   strlcpy(discovery->playlist_directory,
         settings->paths.directory_playlist,
         sizeof(discovery->playlist_directory));

   if (!content_discovery_parse_config(discovery, rules_path,
            settings->paths.directory_libretro))
      goto error;

   discovery->status = CONTENT_DISCOVERY_BEGIN_RULE;

   task->handler = content_discovery_task_handler;
   task->state = discovery;
   task->title = strdup("Scanning games...");
   task->progress = 0;
   task->callback = content_discovery_task_callback;
   task->cleanup = content_discovery_task_free;
   task->flags |= RETRO_TASK_FLG_ALTERNATIVE_LOOK;

   if (!task_queue_push(task))
      goto error;

   content_discovery_notify("Scanning games...", 1, 120,
         MESSAGE_QUEUE_CATEGORY_INFO);
   RARCH_LOG("[Content Discovery] Started with %u root(s), %u rule(s): %s\n",
         (unsigned)discovery->root_count, (unsigned)discovery->rule_count,
         rules_path);
   return true;

error:
   if (task)
   {
      if (task->title)
         task_free_title(task);
      free(task);
   }
   content_discovery_handle_free(discovery);
   content_discovery_notify("Game scan configuration error.", 2, 240,
         MESSAGE_QUEUE_CATEGORY_ERROR);
   RARCH_ERR("[Content Discovery] Cannot start using rules: %s\n",
         rules_path ? rules_path : "(none)");
   return false;
}
