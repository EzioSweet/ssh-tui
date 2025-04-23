#include <stdio.h>
#include <ncurses.h>
#include <string.h>
#include <stdlib.h>
#include <locale.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#include <libgen.h>
#include <ctype.h>
#define PROJECT_NAME "scp-tui"
#define MAX_HOSTS 128
#define MAX_HOSTNAME_LEN 128
#define MAX_FILES 1024
#define MAX_FILENAME_LEN 256
#define COLOR_PAIR_DEFAULT 1
#define COLOR_PAIR_HIGHLIGHT 2
#define COLOR_PAIR_TITLE 3
#define COLOR_PAIR_BORDER 4
#define COLOR_PAIR_DIRECTORY 5
#define COLOR_PAIR_SELECTED 6
#define COLOR_PAIR_PROGRESS 7

static int show_hidden_files = 0;

typedef struct {
    int is_active;
    int progress;
    char source[PATH_MAX];
    char dest[PATH_MAX];
    char hostname[MAX_HOSTNAME_LEN];
    int direction;
    pthread_t thread;
    int cancel_requested;
    FILE *pipe;
} TransferStatus;

static TransferStatus current_transfer = {0};

typedef struct {
    char name[MAX_FILENAME_LEN];
    int is_dir;
    int selected;
} FileEntry;

typedef struct {
    FileEntry files[MAX_FILES];
    int count;
    int selected;
    int scroll_offset;
    char cwd[PATH_MAX];
} FileList;

int compare_file_entries(const void *a, const void *b) {
    const FileEntry *fa = (const FileEntry *)a;
    const FileEntry *fb = (const FileEntry *)b;
    
    if (strcmp(fa->name, "..") == 0) return -1;
    if (strcmp(fb->name, "..") == 0) return 1;
    
    if (fa->is_dir && !fb->is_dir) return -1;
    if (!fa->is_dir && fb->is_dir) return 1;
    
    return strcasecmp(fa->name, fb->name);
}

int parse_ssh_config(char hosts[][MAX_HOSTNAME_LEN], int max_hosts) {
    char config_path[256];
    const char *home = getenv("HOME");
    if (home) {
        snprintf(config_path, sizeof(config_path), "%s/.ssh/config", home);
    } else {
        strncpy(config_path, "~/.ssh/config", sizeof(config_path)-1);
        config_path[sizeof(config_path)-1] = '\0';
    }
    FILE *fp = fopen(config_path, "r");
    if (!fp) return 0;
    char line[256];
    int count = 0;
    while (fgets(line, sizeof(line), fp) && count < max_hosts) {
        if (strncmp(line, "Host ", 5) == 0) {
            char *host = line + 5;
            while (*host == ' ' || *host == '\t') host++;
            char *end = host;
            while (*end && *end != ' ' && *end != '\t' && *end != '\n') end++;
            *end = '\0';
            strncpy(hosts[count++], host, MAX_HOSTNAME_LEN-1);
            hosts[count-1][MAX_HOSTNAME_LEN-1] = '\0';
        }
    }
    fclose(fp);
    return count;
}

void read_local_dir(FileList *list, const char *path) {
    DIR *dir = opendir(path);
    if (!dir) return;
    struct dirent *entry;
    list->count = 0;
    
    strncpy(list->files[list->count].name, "..", MAX_FILENAME_LEN-1);
    list->files[list->count].name[MAX_FILENAME_LEN-1] = '\0';
    list->files[list->count].is_dir = 1;
    list->count++;
    
    strncpy(list->cwd, path, PATH_MAX-1);
    list->cwd[PATH_MAX-1] = '\0';
    
    while ((entry = readdir(dir)) && list->count < MAX_FILES) {
        if (strcmp(entry->d_name, ".") == 0) continue;
        if (strcmp(entry->d_name, "..") == 0) continue;
        if (!show_hidden_files && entry->d_name[0] == '.') continue;
        
        strncpy(list->files[list->count].name, entry->d_name, MAX_FILENAME_LEN-1);
        list->files[list->count].name[MAX_FILENAME_LEN-1] = '\0';
        
        char full_path[PATH_MAX];
        snprintf(full_path, PATH_MAX, "%s/%s", path, entry->d_name);
        DIR *check = opendir(full_path);
        list->files[list->count].is_dir = (check != NULL);
        if (check) closedir(check);
        
        list->count++;
    }
    closedir(dir);
    
    if (list->count > 1) {
        qsort(&list->files[0], list->count, sizeof(FileEntry), compare_file_entries);
    }
    
    list->selected = 0;
    list->scroll_offset = 0;
}

int read_remote_dir(FileList *list, const char *host, const char *path) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd), 
             "ssh %s 'cd \"%s\" 2>/dev/null && ls -la | awk \"NR>2 {printf \\\"%%s|%%s\\n\\\", \\$1, \\$NF}\"'", 
             host, path);
    
    FILE *fp = popen(cmd, "r");
    if (!fp) return 0;
    
    list->count = 0;
    
    strncpy(list->files[list->count].name, "..", MAX_FILENAME_LEN-1);
    list->files[list->count].name[MAX_FILENAME_LEN-1] = '\0';
    list->files[list->count].is_dir = 1;
    list->count++;
    
    strncpy(list->cwd, path, PATH_MAX-1);
    list->cwd[PATH_MAX-1] = '\0';
    
    char line[MAX_FILENAME_LEN * 2];
    while (fgets(line, sizeof(line), fp) && list->count < MAX_FILES) {
        char permissions[11];
        
        char *perm_end = strchr(line, '|');
        if (!perm_end) continue;
        
        *perm_end = '\0';
        strncpy(permissions, line, sizeof(permissions)-1);
        permissions[sizeof(permissions)-1] = '\0';
        
        char *name_start = perm_end + 1;
        size_t len = strlen(name_start);
        if (len > 0 && name_start[len-1] == '\n')
            name_start[len-1] = '\0';
            
        if (strcmp(name_start, ".") == 0 || strcmp(name_start, "..") == 0)
            continue;
        if (!show_hidden_files && name_start[0] == '.') continue;
        
        strncpy(list->files[list->count].name, name_start, MAX_FILENAME_LEN-1);
        list->files[list->count].name[MAX_FILENAME_LEN-1] = '\0';
        
        list->files[list->count].is_dir = (permissions[0] == 'd');
        
        list->count++;
    }
    pclose(fp);
    
    if (list->count > 1) {
        qsort(&list->files[0], list->count, sizeof(FileEntry), compare_file_entries);
    }
    
    list->selected = 0;
    list->scroll_offset = 0;
    return 1;
}

void draw_file_list(WINDOW *win, FileList *list, int focus, int width, int height, const char *title) {
    werase(win);
    box(win, 0, 0);
    mvwprintw(win, 0, (width - strlen(title)) / 2, "%s", title);
    
    int display_count = height - 2;
    
    for (int i = 0; i < display_count; ++i) {
        int file_index = list->scroll_offset + i;
        if (file_index >= list->count) break;
        
        int screen_y = i + 1;
        
        if (file_index == list->selected && focus) {
            wattron(win, A_REVERSE);
        }
        
        char prefix[3] = "  ";
        if (list->files[file_index].selected) {
            strcpy(prefix, "* ");
        }
        
        if (list->files[file_index].is_dir) {
            wattron(win, COLOR_PAIR(COLOR_PAIR_DIRECTORY));
            mvwprintw(win, screen_y, 1, "%s%-*s", prefix, width-4, list->files[file_index].name);
            wattroff(win, COLOR_PAIR(COLOR_PAIR_DIRECTORY));
        } else {
            if (list->files[file_index].selected) {
                wattron(win, COLOR_PAIR(COLOR_PAIR_SELECTED));
            }
            mvwprintw(win, screen_y, 1, "%s%-*s", prefix, width-4, list->files[file_index].name);
            if (list->files[file_index].selected) {
                wattroff(win, COLOR_PAIR(COLOR_PAIR_SELECTED));
            }
        }
        
        if (file_index == list->selected && focus) {
            wattroff(win, A_REVERSE);
        }
    }
    wrefresh(win);
}

void resolve_remote_path(char *path, size_t size, const char *host) {
    if (strncmp(path, "~/", 2) == 0 || strcmp(path, "~") == 0) {
        char cmd[512], home[PATH_MAX] = "";
        
        snprintf(cmd, sizeof(cmd), "ssh %s 'echo $HOME' 2>/dev/null", host);
        FILE *fp = popen(cmd, "r");
        if (fp) {
            if (fgets(home, sizeof(home), fp)) {
                size_t len = strlen(home);
                if (len > 0 && home[len-1] == '\n')
                    home[len-1] = '\0';
            }
            pclose(fp);
        }
        
        if (!home[0]) {
            char username[64] = "";
            snprintf(cmd, sizeof(cmd), "ssh %s 'whoami' 2>/dev/null", host);
            fp = popen(cmd, "r");
            if (fp) {
                if (fgets(username, sizeof(username), fp)) {
                    size_t len = strlen(username);
                    if (len > 0 && username[len-1] == '\n')
                        username[len-1] = '\0';
                }
                pclose(fp);
                
                if (strcmp(username, "root") == 0) {
                    strcpy(home, "/root");
                } else {
                    snprintf(home, sizeof(home), "/home/%s", username);
                }
            }
        }
        
        if (strcmp(path, "~") == 0) {
            strncpy(path, home, size-1);
            path[size-1] = '\0';
        } else {
            char temp[PATH_MAX];
            snprintf(temp, sizeof(temp), "%s%s", home, path+1);
            strncpy(path, temp, size-1);
            path[size-1] = '\0';
        }
    }
}

void *monitor_transfer_progress(void *arg) {
    TransferStatus *ts = (TransferStatus *)arg;
    char buffer[1024];
    int percentage = 0;
    
    while (ts->pipe && fgets(buffer, sizeof(buffer), ts->pipe)) {
        if (ts->cancel_requested) break;
        
        char *percent_pos = strstr(buffer, "%");
        if (percent_pos && percent_pos > buffer) {
            char *digit_pos = percent_pos - 1;
            while (digit_pos > buffer && isdigit(*(digit_pos - 1))) {
                digit_pos--;
            }
            
            if (digit_pos < percent_pos) {
                char percent_str[32] = {0};
                strncpy(percent_str, digit_pos, percent_pos - digit_pos);
                percent_str[percent_pos - digit_pos] = '\0';
                percentage = atoi(percent_str);
                
                if (percentage >= 0 && percentage <= 100) {
                    ts->progress = percentage;
                }
            }
        }
    }

    if (!ts->cancel_requested) {
        ts->progress = 100;
    }
    
    ts->is_active = 0;
    if (ts->pipe) {
        pclose(ts->pipe);
        ts->pipe = NULL;
    }
    return NULL;
}

int start_file_transfer(TransferStatus *ts) {
    char cmd[2048];
    
    if (ts->direction == 1) {
        snprintf(cmd, sizeof(cmd), 
                 "scp -v -p \"%s\" %s:\"%s\" 2>&1", 
                 ts->source, ts->hostname, ts->dest);
    } else {
        snprintf(cmd, sizeof(cmd), 
                 "scp -v -p %s:\"%s\" \"%s\" 2>&1", 
                 ts->hostname, ts->source, ts->dest);
    }
    
    ts->pipe = popen(cmd, "r");
    if (!ts->pipe) {
        return 0;
    }
    
    ts->is_active = 1;
    ts->progress = 0;
    ts->cancel_requested = 0;
    
    if (pthread_create(&ts->thread, NULL, monitor_transfer_progress, ts) != 0) {
        pclose(ts->pipe);
        ts->pipe = NULL;
        ts->is_active = 0;
        return 0;
    }
    
    return 1;
}

void cancel_transfer(TransferStatus *ts) {
    if (ts->is_active) {
        ts->cancel_requested = 1;
        if (ts->pipe) {
            int pid = -1;
            char cmd[64];
            snprintf(cmd, sizeof(cmd), "ps -o pid= -C scp");
            FILE *fp = popen(cmd, "r");
            if (fp) {
                if (fscanf(fp, "%d", &pid) == 1 && pid > 0) {
                    kill(pid, SIGTERM);
                }
                pclose(fp);
            }
        }
        pthread_join(ts->thread, NULL);
        ts->is_active = 0;
        if (ts->pipe) {
            pclose(ts->pipe);
            ts->pipe = NULL;
        }
    }
}

int file_exists(const char *path) {
    return access(path, F_OK) != -1;
}

int remote_file_exists(const char *host, const char *path) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "ssh %s '[ -e \"%s\" ] && echo \"exists\" || echo \"not exists\"'", host, path);
    FILE *fp = popen(cmd, "r");
    if (!fp) return 0;
    
    char result[20];
    int exists = 0;
    if (fgets(result, sizeof(result), fp)) {
        exists = (strncmp(result, "exists", 6) == 0);
    }
    pclose(fp);
    return exists;
}

void draw_progress_bar(WINDOW *win, int progress, const char *message) {
    int width = getmaxx(win) - 4;
    int filled = (progress * width) / 100;
    
    werase(win);
    box(win, 0, 0);
    
    if (message && *message) {
        mvwprintw(win, 0, 2, " %s ", message);
    }
    
    wmove(win, 1, 2);
    for (int i = 0; i < width; i++) {
        waddch(win, ACS_CKBOARD);
    }
    
    wattron(win, COLOR_PAIR(COLOR_PAIR_PROGRESS) | A_REVERSE);
    for (int i = 0; i < filled; i++) {
        mvwaddch(win, 1, 2 + i, ' ');
    }
    wattroff(win, COLOR_PAIR(COLOR_PAIR_PROGRESS) | A_REVERSE);
    
    char percent_str[10];
    snprintf(percent_str, sizeof(percent_str), "%3d%%", progress);
    wattron(win, A_BOLD);
    mvwprintw(win, 1, 2 + (width - strlen(percent_str)) / 2, "%s", percent_str);
    wattroff(win, A_BOLD);
    
    wrefresh(win);
}

void file_manager_ui(const char *remote_host) {
    int left_focus = 1;
    FileList local, remote;
    char local_path[PATH_MAX];
    char remote_path[PATH_MAX];
    
    start_color();
    init_pair(COLOR_PAIR_DEFAULT, COLOR_WHITE, COLOR_BLACK);
    init_pair(COLOR_PAIR_HIGHLIGHT, COLOR_BLACK, COLOR_CYAN);
    init_pair(COLOR_PAIR_TITLE, COLOR_YELLOW, COLOR_BLACK);
    init_pair(COLOR_PAIR_BORDER, COLOR_BLUE, COLOR_BLACK);
    init_pair(COLOR_PAIR_DIRECTORY, COLOR_GREEN, COLOR_BLACK);
    init_pair(COLOR_PAIR_SELECTED, COLOR_CYAN, COLOR_BLACK);
    init_pair(COLOR_PAIR_PROGRESS, COLOR_BLUE, COLOR_BLACK);
    
    const char *home = getenv("HOME");
    if (home)
        strncpy(local_path, home, PATH_MAX-1);
    else
        strcpy(local_path, ".");
    local_path[PATH_MAX-1] = '\0';
    
    strcpy(remote_path, "~");
    resolve_remote_path(remote_path, sizeof(remote_path), remote_host);
    
    read_local_dir(&local, local_path);
    read_remote_dir(&remote, remote_host, remote_path);

    int win_height = LINES - 5;
    int win_width = COLS / 2 - 2;
    WINDOW *left = newwin(win_height, win_width, 1, 1);
    WINDOW *right = newwin(win_height, win_width, 1, COLS/2 + 1);
    keypad(left, TRUE);
    keypad(right, TRUE);
    
    WINDOW *progress_win = newwin(1, COLS-2, LINES-3, 1);
    
    WINDOW *status = newwin(1, COLS, LINES-2, 0);
    mvwprintw(status, 0, 1,
        "Tab: Switch panel | Enter: Open directory | Space: Select file | F5: Download | F6: Upload | P: Show hidden files | Q: Quit");
    wrefresh(status);

    int ch;
    while (1) {
        move(0, 0);
        clrtoeol();
        mvprintw(0, 1, "Local: %s", local.cwd);
        mvprintw(0, COLS/2 + 1, "Remote: %s", remote.cwd);
        refresh();
        
        if (current_transfer.is_active) {
            char message[256];
            if (current_transfer.direction == 1) {
                snprintf(message, sizeof(message), "Uploading: %s", basename(current_transfer.source));
            } else {
                snprintf(message, sizeof(message), "Downloading: %s", basename(current_transfer.source));
            }
            draw_progress_bar(progress_win, current_transfer.progress, message);
        } else {
            werase(progress_win);
            box(progress_win, 0, 0);
            wrefresh(progress_win);
        }
        
        draw_file_list(left, &local, left_focus, win_width, win_height, "Local");
        draw_file_list(right, &remote, !left_focus, win_width, win_height, "Remote");
        
        if (left_focus)
            ch = wgetch(left);
        else
            ch = wgetch(right);
            
        if (ch == 'q' || ch == 'Q') {
            if (current_transfer.is_active) {
                mvwprintw(status, 0, 1, "Transfer in progress, confirm exit? (y/n)");
                wrefresh(status);
                int confirm = wgetch(status);
                if (confirm == 'y' || confirm == 'Y') {
                    cancel_transfer(&current_transfer);
                    break;
                } else {
                    mvwprintw(status, 0, 1,
                        "Tab: Switch panel | Enter: Open directory | Space: Select file | F5: Download | F6: Upload | P: Show hidden files | Q: Quit");
                    wclrtoeol(status);
                    wrefresh(status);
                    continue;
                }
            } else {
                break;
            }
        }
        
        if (ch == '\t' || ch == KEY_LEFT || ch == KEY_RIGHT) {
            left_focus = !left_focus;
        } else if (ch == KEY_UP) {
            FileList *fl = left_focus ? &local : &remote;
            if (fl->selected > 0) {
                fl->selected--;
                if (fl->selected < fl->scroll_offset) {
                    fl->scroll_offset = fl->selected;
                }
            }
        } else if (ch == KEY_DOWN) {
            FileList *fl = left_focus ? &local : &remote;
            if (fl->selected < fl->count-1) {
                fl->selected++;
                int display_count = win_height - 2;
                if (fl->selected >= fl->scroll_offset + display_count) {
                    fl->scroll_offset = fl->selected - display_count + 1;
                }
            }
        } else if (ch == 10 || ch == KEY_ENTER || ch == '\n') {
            FileList *fl = left_focus ? &local : &remote;
            int idx = fl->selected;
            char *sel = fl->files[idx].name;
            
            if (strcmp(sel, "..") == 0) {
                char *slash = strrchr(fl->cwd, '/');
                if (slash && slash != fl->cwd) {
                    *slash = '\0';
                } else {
                    strcpy(fl->cwd, "/");
                }
            } else {
                if (fl->files[idx].is_dir) {
                    char new_path[PATH_MAX];
                    if (strcmp(fl->cwd, "/") == 0) {
                        snprintf(new_path, sizeof(new_path), "/%s", sel);
                    } else {
                        snprintf(new_path, sizeof(new_path), "%s/%s", fl->cwd, sel);
                    }
                    
                    strncpy(fl->cwd, new_path, PATH_MAX-1);
                    fl->cwd[PATH_MAX-1] = '\0';
                }
            }
            
            if (left_focus) {
                read_local_dir(&local, fl->cwd);
            } else {
                read_remote_dir(&remote, remote_host, fl->cwd);
            }
        } else if (ch == ' ') {
            FileList *fl = left_focus ? &local : &remote;
            int idx = fl->selected;
            
            if (strcmp(fl->files[idx].name, "..") != 0) {
                fl->files[idx].selected = !fl->files[idx].selected;
            }
        } else if (ch == KEY_F(5)) {
            if (current_transfer.is_active) {
                mvwprintw(status, 0, 1, "A transfer is already in progress, please wait for it to complete");
                wclrtoeol(status);
                wrefresh(status);
                napms(1500);
                mvwprintw(status, 0, 1,
                    "Tab: Switch panel | Enter: Open directory | Space: Select file | F5: Download | F6: Upload | P: Show hidden files | Q: Quit");
                wclrtoeol(status);
                wrefresh(status);
                continue;
            }
            
            int has_selected = 0;
            for (int i = 0; i < remote.count; i++) {
                if (remote.files[i].selected) {
                    has_selected = 1;
                    break;
                }
            }
            
            if (!has_selected && remote.selected > 0 && !remote.files[remote.selected].is_dir) {
                remote.files[remote.selected].selected = 1;
                has_selected = 1;
            }
            
            if (has_selected) {
                for (int i = 0; i < remote.count; i++) {
                    if (!remote.files[i].selected) continue;
                    
                    char src_path[PATH_MAX], dest_path[PATH_MAX];
                    
                    if (strcmp(remote.cwd, "/") == 0) {
                        snprintf(src_path, sizeof(src_path), "/%s", remote.files[i].name);
                    } else {
                        snprintf(src_path, sizeof(src_path), "%s/%s", remote.cwd, remote.files[i].name);
                    }
                    
                    snprintf(dest_path, sizeof(dest_path), "%s/%s", local.cwd, remote.files[i].name);
                    
                    if (file_exists(dest_path)) {
                        mvwprintw(status, 0, 1, "File %s already exists, overwrite? (y/n)", remote.files[i].name);
                        wclrtoeol(status);
                        wrefresh(status);
                        int confirm = wgetch(status);
                        if (confirm != 'y' && confirm != 'Y') {
                            remote.files[i].selected = 0;
                            continue;
                        }
                    }
                    
                    strncpy(current_transfer.source, src_path, sizeof(current_transfer.source)-1);
                    strncpy(current_transfer.dest, dest_path, sizeof(current_transfer.dest)-1);
                    strncpy(current_transfer.hostname, remote_host, sizeof(current_transfer.hostname)-1);
                    current_transfer.direction = 0;
                    
                    if (start_file_transfer(&current_transfer)) {
                        while (current_transfer.is_active) {
                            char message[256];
                            snprintf(message, sizeof(message), "Downloading %s", remote.files[i].name);
                            draw_progress_bar(progress_win, current_transfer.progress, message);
                            wrefresh(progress_win);
                            napms(100);
                            
                            nodelay(status, TRUE);
                            int key = wgetch(status);
                            nodelay(status, FALSE);
                            
                            if (key == 'q' || key == 'Q') {
                                mvwprintw(status, 0, 1, "Cancel transfer? (y/n)");
                                wclrtoeol(status);
                                wrefresh(status);
                                int confirm = wgetch(status);
                                if (confirm == 'y' || confirm == 'Y') {
                                    cancel_transfer(&current_transfer);
                                    break;
                                } else {
                                    mvwprintw(status, 0, 1,
                                        "Tab: Switch panel | Enter: Open directory | Space: Select file | F5: Download | F6: Upload | P: Show hidden files | Q: Quit");
                                    wclrtoeol(status);
                                    wrefresh(status);
                                }
                            }
                        }
                    } else {
                        mvwprintw(status, 0, 1, "Unable to start transfer %s", remote.files[i].name);
                        wclrtoeol(status);
                        wrefresh(status);
                        napms(1500);
                    }
                    
                    remote.files[i].selected = 0;
                }
                
                read_local_dir(&local, local.cwd);
                
                werase(progress_win);
                box(progress_win, 0, 0);
                mvwprintw(progress_win, 1, 2, "Transfer complete");
                wrefresh(progress_win);
                
                mvwprintw(status, 0, 1,
                    "Tab: Switch panel | Enter: Open directory | Space: Select file | F5: Download | F6: Upload | P: Show hidden files | Q: Quit");
                wclrtoeol(status);
                wrefresh(status);
            } else {
                mvwprintw(status, 0, 1, "Please select a file to download");
                wclrtoeol(status);
                wrefresh(status);
                napms(1500);
                
                mvwprintw(status, 0, 1,
                    "Tab: Switch panel | Enter: Open directory | Space: Select file | F5: Download | F6: Upload | P: Show hidden files | Q: Quit");
                wclrtoeol(status);
                wrefresh(status);
            }
        } else if (ch == KEY_F(6)) {
            if (current_transfer.is_active) {
                mvwprintw(status, 0, 1, "A transfer is already in progress, please wait for it to complete");
                wclrtoeol(status);
                wrefresh(status);
                napms(1500);
                mvwprintw(status, 0, 1,
                    "Tab: Switch panel | Enter: Open directory | Space: Select file | F5: Download | F6: Upload | P: Show hidden files | Q: Quit");
                wclrtoeol(status);
                wrefresh(status);
                continue;
            }
            
            int has_selected = 0;
            for (int i = 0; i < local.count; i++) {
                if (local.files[i].selected) {
                    has_selected = 1;
                    break;
                }
            }
            
            if (!has_selected && local.selected > 0 && !local.files[local.selected].is_dir) {
                local.files[local.selected].selected = 1;
                has_selected = 1;
            }
            
            if (has_selected) {
                for (int i = 0; i < local.count; i++) {
                    if (!local.files[i].selected) continue;
                    
                    char src_path[PATH_MAX], dest_path[PATH_MAX];
                    
                    if (strcmp(local.cwd, "/") == 0) {
                        snprintf(src_path, sizeof(src_path), "/%s", local.files[i].name);
                    } else {
                        snprintf(src_path, sizeof(src_path), "%s/%s", local.cwd, local.files[i].name);
                    }
                    
                    if (strcmp(remote.cwd, "/") == 0) {
                        snprintf(dest_path, sizeof(dest_path), "/%s", local.files[i].name);
                    } else {
                        snprintf(dest_path, sizeof(dest_path), "%s/%s", remote.cwd, local.files[i].name);
                    }
                    
                    if (remote_file_exists(remote_host, dest_path)) {
                        mvwprintw(status, 0, 1, "File %s already exists, overwrite? (y/n)", local.files[i].name);
                        wclrtoeol(status);
                        wrefresh(status);
                        int confirm = wgetch(status);
                        if (confirm != 'y' && confirm != 'Y') {
                            local.files[i].selected = 0;
                            continue;
                        }
                    }
                    
                    strncpy(current_transfer.source, src_path, sizeof(current_transfer.source)-1);
                    strncpy(current_transfer.dest, dest_path, sizeof(current_transfer.dest)-1);
                    strncpy(current_transfer.hostname, remote_host, sizeof(current_transfer.hostname)-1);
                    current_transfer.direction = 1;
                    
                    if (start_file_transfer(&current_transfer)) {
                        while (current_transfer.is_active) {
                            char message[256];
                            snprintf(message, sizeof(message), "Uploading %s", local.files[i].name);
                            draw_progress_bar(progress_win, current_transfer.progress, message);
                            wrefresh(progress_win);
                            napms(100);
                            
                            nodelay(status, TRUE);
                            int key = wgetch(status);
                            nodelay(status, FALSE);
                            
                            if (key == 'q' || key == 'Q') {
                                mvwprintw(status, 0, 1, "Cancel transfer? (y/n)");
                                wclrtoeol(status);
                                wrefresh(status);
                                int confirm = wgetch(status);
                                if (confirm == 'y' || confirm == 'Y') {
                                    cancel_transfer(&current_transfer);
                                    break;
                                } else {
                                    mvwprintw(status, 0, 1,
                                        "Tab: Switch panel | Enter: Open directory | Space: Select file | F5: Download | F6: Upload | P: Show hidden files | Q: Quit");
                                    wclrtoeol(status);
                                    wrefresh(status);
                                }
                            }
                        }
                    } else {
                        mvwprintw(status, 0, 1, "Unable to start transfer %s", local.files[i].name);
                        wclrtoeol(status);
                        wrefresh(status);
                        napms(1500);
                    }
                    
                    local.files[i].selected = 0;
                }
                
                read_remote_dir(&remote, remote_host, remote.cwd);
                
                werase(progress_win);
                box(progress_win, 0, 0);
                mvwprintw(progress_win, 1, 2, "Transfer complete");
                wrefresh(progress_win);
                
                mvwprintw(status, 0, 1,
                    "Tab: Switch panel | Enter: Open directory | Space: Select file | F5: Download | F6: Upload | P: Show hidden files | Q: Quit");
                wclrtoeol(status);
                wrefresh(status);
            } else {
                mvwprintw(status, 0, 1, "Please select a file to upload");
                wclrtoeol(status);
                wrefresh(status);
                napms(1500);
                
                mvwprintw(status, 0, 1,
                    "Tab: Switch panel | Enter: Open directory | Space: Select file | F5: Download | F6: Upload | P: Show hidden files | Q: Quit");
                wclrtoeol(status);
                wrefresh(status);
            }
        } else if (ch == 'p' || ch == 'P') {
            show_hidden_files = !show_hidden_files;
            read_local_dir(&local, local.cwd);
            read_remote_dir(&remote, remote_host, remote.cwd);
        }
    }
    
    if (current_transfer.is_active) {
        cancel_transfer(&current_transfer);
    }
    
    delwin(left);
    delwin(right);
    delwin(status);
    delwin(progress_win);
}

void print_menu(WINDOW *menu_win, int highlight, char hosts[][MAX_HOSTNAME_LEN], int host_count, int win_width) {
    int x = 2;
    int y = 2;
    wattron(menu_win, COLOR_PAIR(COLOR_PAIR_BORDER));
    box(menu_win, 0 , 0);
    wattroff(menu_win, COLOR_PAIR(COLOR_PAIR_BORDER));

    wattron(menu_win, COLOR_PAIR(COLOR_PAIR_TITLE));
    mvwprintw(menu_win, 0, (win_width - strlen("Select SSH Host")) / 2, "Select SSH Host");
    wattroff(menu_win, COLOR_PAIR(COLOR_PAIR_TITLE));

    for (int i = 0; i < host_count; ++i) {
        char display_name[win_width - 3];
        snprintf(display_name, sizeof(display_name), "%s", hosts[i]);
        if (highlight == i + 1) {
            wattron(menu_win, COLOR_PAIR(COLOR_PAIR_HIGHLIGHT));
            mvwprintw(menu_win, y, x, "%-*s", win_width - 4, display_name);
            wattroff(menu_win, COLOR_PAIR(COLOR_PAIR_HIGHLIGHT));
        } else {
            wattron(menu_win, COLOR_PAIR(COLOR_PAIR_DEFAULT));
            mvwprintw(menu_win, y, x, "%-*s", win_width - 4, display_name);
            wattroff(menu_win, COLOR_PAIR(COLOR_PAIR_DEFAULT));
        }
        ++y;
    }
    wrefresh(menu_win);
}

int select_host(char hosts[][MAX_HOSTNAME_LEN], int host_count) {
    setlocale(LC_ALL, "C");
    initscr();
    clear();
    noecho();
    cbreak();
    curs_set(0);
    if (has_colors() == FALSE) {
        endwin();
        printf("Your terminal does not support color\n");
        exit(1);
    }
    start_color();
    init_pair(COLOR_PAIR_DEFAULT, COLOR_WHITE, COLOR_BLACK);
    init_pair(COLOR_PAIR_HIGHLIGHT, COLOR_BLACK, COLOR_CYAN);
    init_pair(COLOR_PAIR_TITLE, COLOR_YELLOW, COLOR_BLACK);
    init_pair(COLOR_PAIR_BORDER, COLOR_BLUE, COLOR_BLACK);

    int max_name_len = 0;
    for (int i = 0; i < host_count; ++i) {
        int len = strlen(hosts[i]);
        if (len > max_name_len) max_name_len = len;
    }
    int win_height = host_count + 4;
    int win_width = max_name_len + 6;
    int title_width = strlen("Select SSH Host") + 4;
    if (win_width < title_width) win_width = title_width;
    if (win_width > COLS - 2) win_width = COLS - 2;
    int required_height = win_height + 1;
    if (required_height > LINES - 1) win_height = LINES - 2;
    int startx = (COLS - win_width) / 2;
    int starty = (LINES - win_height) / 2;
    if (starty < 0) starty = 0;

    WINDOW *menu_win = newwin(win_height, win_width, starty, startx);
    keypad(menu_win, TRUE);
    wbkgd(menu_win, COLOR_PAIR(COLOR_PAIR_DEFAULT));

    const char *help_text = "Use Up/Down, Enter to select, 'q' to quit.";
    int help_text_len = strlen(help_text);
    int help_text_x = (COLS - help_text_len) / 2;
    int help_text_y = starty + win_height + 1;
    if (help_text_y >= LINES) help_text_y = LINES - 1;
    if (help_text_x < 0) help_text_x = 0;
    attron(COLOR_PAIR(COLOR_PAIR_DEFAULT));
    move(help_text_y, 0);
    clrtoeol();
    mvprintw(help_text_y, help_text_x, "%s", help_text);
    attroff(COLOR_PAIR(COLOR_PAIR_DEFAULT));
    refresh();

    int highlight = 1;
    int choice = 0;
    int c;
    print_menu(menu_win, highlight, hosts, host_count, win_width);
    while (1) {
        c = wgetch(menu_win);
        switch (c) {
            case KEY_UP:
                if (highlight == 1)
                    highlight = host_count;
                else
                    --highlight;
                break;
            case KEY_DOWN:
                if (highlight == host_count)
                    highlight = 1;
                else
                    ++highlight;
                break;
            case 10:
            case KEY_ENTER:
                choice = highlight;
                break;
            case 'q':
                choice = -1;
                break;
            default:
                break;
        }
        print_menu(menu_win, highlight, hosts, host_count, win_width);
        if (choice != 0)
            break;
    }
    clrtoeol();
    refresh();
    endwin();
    if (choice > 0 && choice <= host_count) {
        return choice - 1;
    } else {
        return -1;
    }
}

int main(int argc, char **argv) {
    if(argc != 1) {
        printf("%s takes no arguments.\n", argv[0]);
        return 1;
    }
    char hosts[MAX_HOSTS][MAX_HOSTNAME_LEN];
    int host_count = parse_ssh_config(hosts, MAX_HOSTS);
    if (host_count == 0) {
        printf("No servers found, please check ~/.ssh/config\n");
        return 1;
    }
    int selected = select_host(hosts, host_count);
    if (selected >= 0) {
        printf("You selected: %s\n", hosts[selected]);
        
        endwin();
        
        initscr();
        clear();
        refresh();
        cbreak();
        noecho();
        keypad(stdscr, TRUE);
        curs_set(0);
        
        file_manager_ui(hosts[selected]);
        endwin();
    } else {
        printf("Selection cancelled\n");
    }
    return 0;
}
