#include "../utils/cellmap.h"
#include "../utils/error.h"
#include "../utils/logger.h"
#include "../utils/str_replace.h"
#include "router.h"
#include <arpa/inet.h>
#include <sqlite3.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

void respond_http(int client_socket, char **html, long file_size);
char *read_file(const char *filename, long *file_size);
Error fetch_table(sqlite3 *db, char **res);

int callback(void *data, int argc, char **argv, char **azColName);

Error handle_client(int client_socket, sqlite3 *db) {
  char buffer[2048];
  int read_size;

  memset(buffer, 0, sizeof(buffer));

  read_size = recv(client_socket, buffer, sizeof(buffer) - 1, 0);
  if (read_size < 0) {
    perror("Recv failed");
    close(client_socket);
    return WEB_SERVER_ERROR;
  }

  char *method = strtok(buffer, " ");
  char *path = strtok(NULL, " ");
  strtok(NULL, " ");

  if (path == NULL) {
    printf("Path is null\n");
    close(client_socket);
    return WEB_SERVER_ERROR;
  }
  char *file_buffer;
  long file_size;
  get_template(path, &file_buffer, &file_size);
  char *res;
  fetch_table(db, &res);
  char *replaced = str_replace(file_buffer, "%res%", res);
  printf("%s\n", res);

  respond_http(client_socket, &replaced, strlen(replaced));

  return WEB_SERVER_OK;
}

void respond_http(int client_socket, char **html, long file_size) {
  char response[file_size + 100];
  snprintf(response, sizeof(response),
           "HTTP/1.1 200 OK\r\nContent-Length: %lu"
           "charset=UTF-8\r\n\r\n%s",
           strlen(*html), *html);
  write(client_socket, response, strlen(response));
}

void append_to_string(char **str, size_t *size, size_t *used,
                      const char *format, ...) {
  va_list args;
  va_start(args, format);

  // Calculate the needed size
  size_t required_size = vsnprintf(NULL, 0, format, args) + 1;
  va_end(args);

  if (*used + required_size > *size) {
    // Increase the size of the buffer
    *size = (*size + required_size) * 2;
    *str = realloc(*str, *size);
    if (*str == NULL) {
      perror("Failed to reallocate memory");
      exit(EXIT_FAILURE);
    }
  }

  va_start(args, format);
  vsnprintf(*str + *used, required_size, format, args);
  va_end(args);

  *used += required_size - 1;
}

Error fetch_table(sqlite3 *db, char **response) {
  CellMap *cellmap = cellmap_init();
  int rc = sqlite3_exec(
      db,
      "SELECT \"order\", hours, lesson_name, teacher_id, "
      "classroom, weekday FROM timetable WHERE teacher_id = \"xK\" "
      "ORDER BY \"order\" ASC, weekday ASC",
      callback, &cellmap, 0);

  if (rc != SQLITE_OK) {
    print_error("Failed to select data");
    sqlite3_close(db);
    return 1;
  }

  int item_count = cellmap->len;
  Item *items = malloc(item_count * sizeof(Item));

  cellmap_collect(cellmap, items, &item_count);
  qsort(items, item_count, sizeof(Item), compare_cells);

  char *res = NULL;
  size_t res_size = 1024;
  size_t res_used = 0;

  res = malloc(res_size);
  if (res == NULL) {
    perror("Failed to allocate memory");
    return EXIT_FAILURE;
  }
  res[0] = '\0'; // Initialize the string

  for (int i = 1; i < items[0].key.x; ++i) {
    append_to_string(&res, &res_size, &res_used,
                     "<tr class=\"border-b border-gray\"> <td class=\"py-4 "
                     "px-6\">%i</td><td class=\"py-4 px-6\"></td>",
                     i);
    for (int j = 0; j < 5; ++j) {
      append_to_string(&res, &res_size, &res_used,
                       "<td class=\"py-4 px-6\"></td>");
    }
    append_to_string(&res, &res_size, &res_used, "</tr>");
  }

  for (int i = 0; i < item_count; ++i) {
    if (items[i - 1].key.x != items[i].key.x) {
      append_to_string(&res, &res_size, &res_used,
                       "<tr class=\"border-b border-gray\">");
    }

    LessonArray cell_array = items[i].val;
    if (items[i - 1].key.x != items[i].key.x) {
      append_to_string(
          &res, &res_size, &res_used,
          "<td class=\"py-4 px-6\">%i</td><td class=\"py-4 px-6\">%s</td>",
          cell_array.array[0].order, cell_array.array[0].hours);
      for (int j = 0; j < items[i].key.y; ++j) {
        append_to_string(&res, &res_size, &res_used,
                         "<td class=\"py-4 px-6\"></td>");
      }
    }

    append_to_string(&res, &res_size, &res_used, "<td class=\"py-4 px-6\">");
    for (int j = 0; j < cell_array.count; ++j) {
      append_to_string(&res, &res_size, &res_used, "<span>%s %s %s</span><br/>",
                       cell_array.array[j].lesson_name,
                       cell_array.array[j].teacher_id,
                       cell_array.array[j].classroom);
    }
    append_to_string(&res, &res_size, &res_used, "</td>");

    if (items[i].key.x != items[i + 1].key.x) {
      for (int j = items[i].key.y; j < 4; ++j) {
        append_to_string(&res, &res_size, &res_used,
                         "<td class=\"py-4 px-6\"></td>");
      }
      append_to_string(&res, &res_size, &res_used, "</tr>");
    }
  }
  append_to_string(&res, &res_size, &res_used, "</tr>");

  *response = strdup(res);
  free(res); // Clean up dynamic string buffer
  return 0;
}

int callback(void *data, int argc, char **argv, char **azColName) {
  CellMap **cellmap = (CellMap **)data;
  Error err;

  Lesson lesson = {
      .order = atoi(argv[0]),
      .hours = strdup(argv[1]),
      .lesson_name = strdup(argv[2]),
      .teacher_id = strdup(argv[3]),
      .classroom = strdup(argv[4]),
      .weekday = atoi(argv[5]),
  };

  Cell cell = {.x = lesson.order, .y = lesson.weekday};

  LessonArray out;

  err = cellmap_get(*cellmap, cell, &out);
  if (err != HASHMAP_OPERATION_OK) {
    LessonArray new;
    arrayInit(&new, 8);
    arrayPush(&new, lesson);
    cellmap_set(*cellmap, cell, new);
  } else {
    cellmap_insert_or_push(*cellmap, cell, out, lesson);
  }

  return 0;
}
