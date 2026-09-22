#include "qnx_compat.h"

#if defined(__QNXNTO__) || defined(__QNX__)

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int
vasprintf(char **result, const char *format, va_list args)
{
   va_list copy;
   int length;

   if (!result || !format) {
      errno = EINVAL;
      return -1;
   }
   *result = NULL;
   va_copy(copy, args);
   length = vsnprintf(NULL, 0, format, copy);
   va_end(copy);
   if (length < 0)
      return -1;
   if ((size_t)length == SIZE_MAX) {
      errno = EOVERFLOW;
      return -1;
   }
   *result = malloc((size_t)length + 1);
   if (!*result)
      return -1;
   if (vsnprintf(*result, (size_t)length + 1, format, args) < 0) {
      free(*result);
      *result = NULL;
      return -1;
   }
   return length;
}

int
asprintf(char **result, const char *format, ...)
{
   va_list args;
   int length;

   va_start(args, format);
   length = vasprintf(result, format, args);
   va_end(args);
   return length;
}

int
mkstemps(char *template_name, int suffix_length)
{
   size_t length;
   size_t stem_length;
   char *stem;
   char *final_name;
   int fd;

   if (!template_name || suffix_length < 0) {
      errno = EINVAL;
      return -1;
   }
   length = strlen(template_name);
   if ((size_t)suffix_length > length ||
       length - (size_t)suffix_length < 6) {
      errno = EINVAL;
      return -1;
   }
   stem_length = length - (size_t)suffix_length;
   if (memcmp(template_name + stem_length - 6, "XXXXXX", 6) != 0) {
      errno = EINVAL;
      return -1;
   }

   stem = malloc(stem_length + 1);
   final_name = malloc(length + 1);
   if (!stem || !final_name) {
      free(stem);
      free(final_name);
      return -1;
   }
   memcpy(stem, template_name, stem_length);
   stem[stem_length] = '\0';
   fd = mkstemp(stem);
   if (fd < 0)
      goto out;

   memcpy(final_name, stem, stem_length);
   memcpy(final_name + stem_length, template_name + stem_length,
          (size_t)suffix_length + 1);
   if (rename(stem, final_name) != 0) {
      int saved_errno = errno;
      close(fd);
      unlink(stem);
      fd = -1;
      errno = saved_errno;
      goto out;
   }
   memcpy(template_name, final_name, length + 1);

out:
   free(stem);
   free(final_name);
   return fd;
}

size_t
strnlen(const char *string, size_t max_length)
{
   size_t length = 0;

   if (!string)
      return 0;
   while (length < max_length && string[length])
      length++;
   return length;
}

char *
strndup(const char *string, size_t max_length)
{
   size_t length;
   char *result;

   if (!string) {
      errno = EINVAL;
      return NULL;
   }
   length = strnlen(string, max_length);
   result = malloc(length + 1);
   if (!result)
      return NULL;
   memcpy(result, string, length);
   result[length] = '\0';
   return result;
}

ssize_t
getline(char **line, size_t *capacity, FILE *stream)
{
   size_t length = 0;
   int c;

   if (!line || !capacity || !stream) {
      errno = EINVAL;
      return -1;
   }
   if (!*line || !*capacity) {
      *capacity = 256;
      *line = malloc(*capacity);
      if (!*line)
         return -1;
   }

   while ((c = fgetc(stream)) != EOF) {
      if (length + 1 >= *capacity) {
         size_t next_capacity;
         char *next;

         if (*capacity > SIZE_MAX / 2) {
            errno = EOVERFLOW;
            return -1;
         }
         next_capacity = *capacity * 2;
         next = realloc(*line, next_capacity);
         if (!next)
            return -1;
         *line = next;
         *capacity = next_capacity;
      }
      (*line)[length++] = (char)c;
      if (c == '\n')
         break;
   }
   if (length == 0 && c == EOF)
      return -1;
   (*line)[length] = '\0';
   return (ssize_t)length;
}

#endif
