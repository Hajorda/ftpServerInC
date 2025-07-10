#ifndef COMMANDS_H
#define COMMANDS_H

void handle_client(int sock);
void receive_file(int sock);
void send_file(int sock, const char *filename);
void send_list(int sock);
void send_pwd(int sock);
void change_dir(int sock, const char *path);
void delete_file(int sock, const char *filename);
void rename_file(int sock, const char *old_name, const char *new_name);
void send_health_info(int sock);

#endif
