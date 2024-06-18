#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sqlite3.h>
#include <cjson/cJSON.h>
#include <pthread.h>
#include <fcntl.h>
#include <time.h>

#define PORT 3999
#define BUFFER_SIZE 5000
#define LOG_FILE "server.log"

// Function declarations
void handle_request(int new_socket);
void query_database(char *response);
void insert_into_database(const char *name, char *response);
void update_database(int id, const char *name, char *response);
void delete_from_database(int id, char *response);
void send_response(int socket, const char *header, const char *body);
void initialize_database();
void serve_openapi_spec(int new_socket);
void log_request(const char *method, const char *path, const char *status);

int callback(void *data, int argc, char **argv, char **azColName) {
    int i;
    cJSON *json_array = (cJSON *)data;
    cJSON *json_row = cJSON_CreateObject();

    for (i = 0; i < argc; i++) {
        cJSON_AddStringToObject(json_row, azColName[i], argv[i] ? argv[i] : "NULL");
    }

    cJSON_AddItemToArray(json_array, json_row);
    return 0;
}

void *thread_func(void *arg) {
    int new_socket = *(int *)arg;
    free(arg);
    handle_request(new_socket);
    close(new_socket);
    return NULL;
}

int main() {
    int server_fd;
    struct sockaddr_in address;
    int addrlen = sizeof(address);

    initialize_database();

    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        perror("socket failed");
        exit(EXIT_FAILURE);
    }

    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) {
        perror("setsockopt failed");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT);

    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("bind failed");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    if (listen(server_fd, 3) < 0) {
        perror("listen failed");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    printf("Server is listening on port %d\n", PORT);

    while (1) {
        int *new_socket = malloc(sizeof(int));
        if ((*new_socket = accept(server_fd, (struct sockaddr *)&address, (socklen_t*)&addrlen)) < 0) {
            perror("accept failed");
            free(new_socket);
            close(server_fd);
            exit(EXIT_FAILURE);
        }

        pthread_t thread_id;
        if (pthread_create(&thread_id, NULL, thread_func, (void *)new_socket) != 0) {
            perror("pthread_create failed");
            free(new_socket);
        }

        pthread_detach(thread_id);
    }

    close(server_fd);
    return 0;
}

void initialize_database() {
    sqlite3 *db;
    char *err_msg = 0;
    int rc = sqlite3_open("test.db", &db);

    if (rc != SQLITE_OK) {
        fprintf(stderr, "Can't open database: %s\n", sqlite3_errmsg(db));
        sqlite3_close(db);
        exit(EXIT_FAILURE);
    }

    const char *create_table_sql = "CREATE TABLE IF NOT EXISTS test ("
                                   "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                                   "name TEXT NOT NULL);";

    rc = sqlite3_exec(db, create_table_sql, 0, 0, &err_msg);

    if (rc != SQLITE_OK) {
        fprintf(stderr, "SQL error: %s\n", err_msg);
        sqlite3_free(err_msg);
        sqlite3_close(db);
        exit(EXIT_FAILURE);
    }

    sqlite3_close(db);
}

void handle_request(int new_socket) {
    char buffer[BUFFER_SIZE] = {0};
    char method[10], path[100], protocol[10];
    read(new_socket, buffer, BUFFER_SIZE);
    sscanf(buffer, "%s %s %s", method, path, protocol);
    printf("Received request: %s %s %s\n", method, path, protocol);

    if (strcmp(path, "/openapi.yaml") == 0) {
        serve_openapi_spec(new_socket);
        return;
    }

    if (strcmp(method, "GET") == 0) {
        char response[BUFFER_SIZE * 10];
        query_database(response);
        send_response(new_socket, "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n\r\n", response);
        log_request(method, path, "200 OK");
    } else if (strcmp(method, "POST") == 0) {
        char *json_start = strchr(buffer, '{');
        if (json_start == NULL) {
            send_response(new_socket, "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n\r\n", "");
            log_request(method, path, "400 Bad Request");
            return;
        }

        cJSON *json = cJSON_Parse(json_start);
    
        if (json == NULL) {
            send_response(new_socket, "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n\r\n", "");
            log_request(method, path, "400 Bad Request");
            return;
        }

        cJSON *name = cJSON_GetObjectItem(json, "name");

        if (name == NULL || !cJSON_IsString(name)) {
            cJSON_Delete(json);
            send_response(new_socket, "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n\r\n", "");
            log_request(method, path, "400 Bad Request");
            return;
        }

        char response[BUFFER_SIZE];
        insert_into_database(name->valuestring, response);
        cJSON_Delete(json);
        send_response(new_socket, "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n\r\n", response);
        log_request(method, path, "200 OK");
    } else if (strcmp(method, "PUT") == 0) {
        int id;
        if (sscanf(path, "/records/%d", &id) != 1) {
            send_response(new_socket, "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n\r\n", "");
            log_request(method, path, "400 Bad Request");
            return;
        }

        char *json_start = strchr(buffer, '{');
        if (json_start == NULL) {
            send_response(new_socket, "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n\r\n", "");
            log_request(method, path, "400 Bad Request");
            return;
        }

        cJSON *json = cJSON_Parse(json_start);
        if (json == NULL) {
            send_response(new_socket, "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n\r\n", "");
            log_request(method, path, "400 Bad Request");
            return;
        }

        cJSON *name = cJSON_GetObjectItem(json, "name");

        if (name == NULL || !cJSON_IsString(name)) {
            cJSON_Delete(json);
            send_response(new_socket, "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n\r\n", "");
            log_request(method, path, "400 Bad Request");
            return;
        }

        char response[BUFFER_SIZE];
        update_database(id, name->valuestring, response);
        cJSON_Delete(json);
        send_response(new_socket, "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n\r\n", response);
        log_request(method, path, "200 OK");
    } else if (strcmp(method, "DELETE") == 0) {
        int id;
        if (sscanf(path, "/records/%d", &id) != 1) {
            send_response(new_socket, "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n\r\n", "");
            log_request(method, path, "400 Bad Request");
            return;
        }

        char response[BUFFER_SIZE];
        delete_from_database(id, response);
        send_response(new_socket, "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n\r\n", response);
        log_request(method, path, "200 OK");
    } else {
        send_response(new_socket, "HTTP/1.1 405 Method Not Allowed\r\nContent-Length: 0\r\n\r\n", "");
        log_request(method, path, "405 Method Not Allowed");
    }
}

void log_request(const char *method, const char *path, const char *status) {
    FILE *log_file = fopen(LOG_FILE, "a");
    if (log_file == NULL) {
        perror("Failed to open log file");
        return;
    }
    time_t now;
    time(&now);
    char *time_str = ctime(&now);
    time_str[strlen(time_str) - 1] = '\0'; // Remove newline character
    fprintf(log_file, "[%s] %s %s %s\n", time_str, method, path, status);
    fclose(log_file);
}

void query_database(char *response) {
    sqlite3 *db;
    char *err_msg = 0;
    int rc = sqlite3_open("test.db", &db);

    if (rc != SQLITE_OK) {
        snprintf(response, BUFFER_SIZE, "{\"error\": \"Cannot open database: %s\"}", sqlite3_errmsg(db));
        sqlite3_close(db);
        return;
    }

    cJSON *json_array = cJSON_CreateArray();
    rc = sqlite3_exec(db, "SELECT * FROM test;", callback, (void *)json_array, &err_msg);

    if (rc != SQLITE_OK) {
        snprintf(response, BUFFER_SIZE, "{\"error\": \"SQL error: %s\"}", err_msg);
        sqlite3_free(err_msg);
    } else {
        char *json_str = cJSON_Print(json_array);
        strncpy(response, json_str, BUFFER_SIZE - 1);
        free(json_str);
    }

    cJSON_Delete(json_array);
    sqlite3_close(db);
}

void insert_into_database(const char *name, char *response) {
    sqlite3 *db;
    char *err_msg = 0;
    int rc = sqlite3_open("test.db", &db);

    if (rc != SQLITE_OK) {
        snprintf(response, BUFFER_SIZE, "{\"error\": \"Cannot open database: %s\"}", sqlite3_errmsg(db));
        sqlite3_close(db);
        return;
    }

    char sql[BUFFER_SIZE];
    snprintf(sql, BUFFER_SIZE, "INSERT INTO test (name) VALUES ('%s');", name);

    rc = sqlite3_exec(db, sql, 0, 0, &err_msg);

    if (rc != SQLITE_OK) {
        snprintf(response, BUFFER_SIZE, "{\"error\": \"SQL error: %s\"}", err_msg);
        sqlite3_free(err_msg);
    } else {
        snprintf(response, BUFFER_SIZE, "{\"message\": \"Record inserted successfully\"}");
    }

    sqlite3_close(db);
}

void update_database(int id, const char *name, char *response) {
    sqlite3 *db;
    char *err_msg = 0;
    int rc = sqlite3_open("test.db", &db);

    if (rc != SQLITE_OK) {
        snprintf(response, BUFFER_SIZE, "{\"error\": \"Cannot open database: %s\"}", sqlite3_errmsg(db));
        sqlite3_close(db);
        return;
    }

    char sql[BUFFER_SIZE];
    snprintf(sql, BUFFER_SIZE, "UPDATE test SET name = '%s' WHERE id = %d;", name, id);

    rc = sqlite3_exec(db, sql, 0, 0, &err_msg);

    if (rc != SQLITE_OK) {
        snprintf(response, BUFFER_SIZE, "{\"error\": \"SQL error: %s\"}", err_msg);
        sqlite3_free(err_msg);
    } else {
        snprintf(response, BUFFER_SIZE, "{\"message\": \"Record updated successfully\"}");
    }

    sqlite3_close(db);
}

void delete_from_database(int id, char *response) {
    sqlite3 *db;
    char *err_msg = 0;
    int rc = sqlite3_open("test.db", &db);

    if (rc != SQLITE_OK) {
        snprintf(response, BUFFER_SIZE, "{\"error\": \"Cannot open database: %s\"}", sqlite3_errmsg(db));
        sqlite3_close(db);
        return;
    }

    char sql[BUFFER_SIZE];
    snprintf(sql, BUFFER_SIZE, "DELETE FROM test WHERE id = %d;", id);

    rc = sqlite3_exec(db, sql, 0, 0, &err_msg);

    if (rc != SQLITE_OK) {
        snprintf(response, BUFFER_SIZE, "{\"error\": \"SQL error: %s\"}", err_msg);
        sqlite3_free(err_msg);
    } else {
        snprintf(response, BUFFER_SIZE, "{\"message\": \"Record deleted successfully\"}");
    }

    sqlite3_close(db);
}

void send_response(int socket, const char *header, const char *body) {
    char response[BUFFER_SIZE * 10];
    snprintf(response, sizeof(response), "%s%s", header, body);
    send(socket, response, strlen(response), 0);
}

void serve_openapi_spec(int new_socket) {
    char buffer[BUFFER_SIZE] = {0};
    int fd = open("openapi.yaml", O_RDONLY);
    if (fd < 0) {
        send_response(new_socket, "HTTP/1.1 500 Internal Server Error\r\nContent-Length: 0\r\n\r\n", "");
        return;
    }

    ssize_t bytes_read = read(fd, buffer, sizeof(buffer) - 1);
    close(fd);

    if (bytes_read < 0) {
        send_response(new_socket, "HTTP/1.1 500 Internal Server Error\r\nContent-Length: 0\r\n\r\n", "");
        return;
    }

    send_response(new_socket, "HTTP/1.1 200 OK\r\nContent-Type: application/yaml\r\n\r\n", buffer);
}
