#include <ncurses.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <wordexp.h>
#include <limits.h>
#include <ctype.h>
#include <locale.h>

typedef struct {
    char **items;
    int count;
    int capacity;
} ServerList;

void init_server_list(ServerList *list, int initial_capacity) {
    list->items =(char**) malloc(initial_capacity * sizeof(char*));
    if (list->items == NULL) {
        perror("Failed to allocate memory for server list");
        exit(EXIT_FAILURE);
    }
    list->count = 0;
    list->capacity = initial_capacity;
}

void add_server(ServerList *list, const char *server_name) {
    if (list->count >= list->capacity) {
        list->capacity *= 2;
        char **temp = (char**) realloc(list->items, list->capacity * sizeof(char*));
        if (temp == NULL) {
            perror("Failed to reallocate memory for server list");
            exit(EXIT_FAILURE);
        }
        list->items = temp;
    }
    list->items[list->count] = strdup(server_name);
    if (list->items[list->count] == NULL) {
        perror("Failed to duplicate server name string");
        exit(EXIT_FAILURE);
    }
    list->count++;
}

void free_server_list(ServerList *list) {
    for (int i = 0; i < list->count; ++i) {
        free(list->items[i]);
    }
    free(list->items);
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

int read_ssh_config(ServerList *list) {
    wordexp_t p;
    FILE *fp;
    char *line = NULL;
    size_t len = 0;
    ssize_t read;
    char config_path[PATH_MAX];

    if (wordexp("~/.ssh/config", &p, 0) != 0) {
        fprintf(stderr, "Failed to expand path ~/.ssh/config\n");
        return 0;
    }
    if (p.we_wordc < 1) {
         fprintf(stderr, "wordexp failed unexpectedly\n");
         wordfree(&p);
         return 0;
    }
    strncpy(config_path, p.we_wordv[0], PATH_MAX - 1);
    config_path[PATH_MAX - 1] = '\0';
    wordfree(&p);

    fp = fopen(config_path, "r");
    if (fp == NULL) {
        perror("Could not open SSH config file");
        fprintf(stderr, "Attempted path: %s\n", config_path);
        return 1;
    }

    init_server_list(list, 10);

    while ((read = getline(&line, &len, fp)) != -1) {
        char *trimmed_line = line;
        while (isspace((unsigned char)*trimmed_line)) trimmed_line++;
        if (*trimmed_line == '#' || *trimmed_line == '\0') {
            continue;
        }

        if (strncasecmp(trimmed_line, "Host ", 5) == 0) {
            char *host_start = trimmed_line + 5;
            while (isspace((unsigned char)*host_start)) host_start++;

            char *host_end = host_start + strlen(host_start) - 1;
            while (host_end > host_start && isspace((unsigned char)*host_end)) {
                *host_end = '\0';
                host_end--;
            }

            if (strcmp(host_start, "*") != 0) {
                 char *first_space = strchr(host_start, ' ');
                 if (first_space != NULL) {
                     *first_space = '\0';
                 }
                 if(strlen(host_start) > 0) {
                    add_server(list, host_start);
                 }
            }
        }
    }

    fclose(fp);
    if (line) {
        free(line);
    }
    return 1;
}

#define COLOR_PAIR_DEFAULT 1
#define COLOR_PAIR_HIGHLIGHT 2
#define COLOR_PAIR_TITLE 3
#define COLOR_PAIR_BORDER 4

void print_menu(WINDOW *menu_win, int highlight, const ServerList *list, int win_width) {
    int x = 2;
    int y = 2;
    wattron(menu_win, COLOR_PAIR(COLOR_PAIR_BORDER));
    box(menu_win, 0 , 0);
    wattroff(menu_win, COLOR_PAIR(COLOR_PAIR_BORDER));

    wattron(menu_win, COLOR_PAIR(COLOR_PAIR_TITLE));
    mvwprintw(menu_win, 0, (win_width - strlen("Select SSH Host")) / 2, "Select SSH Host");
    wattroff(menu_win, COLOR_PAIR(COLOR_PAIR_TITLE));

    for (int i = 0; i < list->count; ++i) {
        char display_name[win_width - 3];
        snprintf(display_name, sizeof(display_name), "%s", list->items[i]);

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

int main() {
    WINDOW *menu_win;
    int highlight = 1;
    int choice = 0;
    int c;
    ServerList server_list;
    int max_name_len = 0;

    setlocale(LC_ALL, "");

    if (!read_ssh_config(&server_list)) {
        fprintf(stderr, "Failed to read or parse SSH config.\n");
        return 1;
    }

    if (server_list.count == 0) {
        fprintf(stderr, "No hosts found in SSH config or config file not found/readable.\n");
        free_server_list(&server_list);
        return 0;
    }

    for (int i = 0; i < server_list.count; ++i) {
        int len = strlen(server_list.items[i]);
        if (len > max_name_len) {
            max_name_len = len;
        }
    }

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

    int win_height = server_list.count + 4;
    int win_width = max_name_len + 6;
    int title_width = strlen("Select SSH Host") + 4;
    if (win_width < title_width) {
        win_width = title_width;
    }
    if (win_width > COLS - 2) win_width = COLS - 2;
    int required_height = win_height + 1;
    if (required_height > LINES - 1) {
        win_height = LINES - 2;

    }

    int startx = (COLS - win_width) / 2;
    int starty = (LINES - required_height) / 2;
    if (starty < 0) starty = 0;

    menu_win = newwin(win_height, win_width, starty, startx);
    keypad(menu_win, TRUE);
    wbkgd(menu_win, COLOR_PAIR(COLOR_PAIR_DEFAULT));

    const char *help_text = "Use Ctrl+Q/Ctrl+S or Up/Down, Enter to select, 'q' to quit.";
    int help_text_len = strlen(help_text);
    int help_text_x = (COLS - help_text_len) / 2;
    int help_text_y = starty + win_height + 1;

    if (help_text_y >= LINES) {
        help_text_y = LINES - 1;
    }
    if (help_text_x < 0) help_text_x = 0;

    attron(COLOR_PAIR(COLOR_PAIR_DEFAULT));
    move(help_text_y, 0);
    clrtoeol();
    mvprintw(help_text_y, help_text_x, "%s", help_text);
    attroff(COLOR_PAIR(COLOR_PAIR_DEFAULT));
    refresh();

    print_menu(menu_win, highlight, &server_list, win_width);

    while (1) {
        c = wgetch(menu_win);
        switch (c) {
            case 17:
            case KEY_UP:
                if (highlight == 1)
                    highlight = server_list.count;
                else
                    --highlight;
                break;
            case 19:
            case KEY_DOWN:
                if (highlight == server_list.count)
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
        print_menu(menu_win, highlight, &server_list, win_width);
        if (choice != 0)
            break;
    }

    clrtoeol();
    refresh();
    endwin();

    if (choice > 0 && choice <= server_list.count) {
        char *selected_server = server_list.items[choice - 1];
        char *ssh_argv[3];
        ssh_argv[0] = "ssh";
        ssh_argv[1] = selected_server;
        ssh_argv[2] = NULL;

        printf("Connecting to %s...\n", selected_server);
        execvp("ssh", ssh_argv);
        perror("execvp failed");
        free_server_list(&server_list);
        return 1;
    } else if (choice == -1) {
         printf("Exiting.\n");
    } else {
         printf("No server selected or invalid choice.\n");
    }

    free_server_list(&server_list);

    return 0;
}
