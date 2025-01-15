#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <utime.h>
#include "tree.h"

struct blob *create_blob(const char* file_path)
{
        struct stat st;
        if (lstat(file_path, &st) < 0) {
                perror("lstat");
                return NULL;
        }

        struct blob *blob = malloc(sizeof(struct blob));
        if (!blob) {
                perror("malloc");
                return NULL;
        }

        FILE *file = fopen(file_path, "rb");
        if (!file) {
                perror("fopen");
                free(blob);
                return NULL;
        }

        blob->size = st.st_size;
        blob->data = malloc(st.st_size);
        fread(blob->data, 1, st.st_size, file);
        fclose(file);

        blob->mode = st.st_mode;
        blob->uid = st.st_uid;
        blob->gid = st.st_gid;
        blob->atime = st.st_atim;
        blob->mtime = st.st_mtim;
        blob->ctime = st.st_ctim;
        blob->link_target = NULL;

        return blob;
}

struct tree_entry *create_tree_entry(const char *name, struct blob *blob) {
        struct tree_entry *entry = malloc(sizeof(struct tree_entry));
        if (!entry) {
                perror("malloc");
                return NULL;
        }

        strcpy(entry->name, name);
        entry->mode[0] = '1';
        entry->mode[1] = '0';
        entry->mode[2] = '0';
        entry->mode[3] = '6'; 
        entry->mode[4] = '4';
        entry->mode[5] = '4'; 
        entry->mode[6] = '\0';
        strcpy(entry->type, "blob");
        memcpy(entry->hash, "dummysha1hashofblob", 20);  
        entry->blob = blob;
        entry->next = NULL;

        return entry;
}

struct tree *create_tree(struct tree_entry *entry)
{
        struct tree *tree = malloc(sizeof(struct tree));
        if (!tree) {
                perror("malloc");
                return NULL;
        }

        strcpy(tree->type, "tree");
        tree->entry_count = 1;
        tree->entries = entry;

        return tree;
}

struct tree *form_tree(const char *dir_path)
{
        DIR *dir = opendir(dir_path);
        if (!dir) {
                perror("opendir");
                return NULL;
        }

        struct tree *root_tree = malloc(sizeof(struct tree));
        if (!root_tree) {
                perror("malloc");
                closedir(dir);
                return NULL;
        }

        strcpy(root_tree->type, "tree");
        root_tree->entry_count = 0;
        root_tree->entries = NULL;

        struct dirent *entry;
        struct tree_entry *last_entry = NULL;

        while ((entry = readdir(dir)) != NULL) {
                if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
                        continue;
                }

                char full_path[1024];
                snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, entry->d_name);

                struct stat st;
                if (lstat(full_path, &st) < 0) {
                        perror("lstat");
                        continue;
                }

                if (S_ISDIR(st.st_mode)) {
                        struct tree *subdir_tree = form_tree(full_path);
                        struct tree_entry *subdir_entry = create_tree_entry(entry->d_name, NULL);
                        strcpy(subdir_entry->type, "tree");
                        subdir_entry->blob = NULL;
                        subdir_entry->next = NULL;

                        if (last_entry) {
                                last_entry->next = subdir_entry;
                        } else {
                                root_tree->entries = subdir_entry;
                        }
                        
                        last_entry = subdir_entry;
                        root_tree->entry_count++;
                } else if (S_ISREG(st.st_mode)) {
                        struct blob *file_blob = create_blob(full_path);
                        struct tree_entry *file_entry = create_tree_entry(entry->d_name, file_blob);

                        if (last_entry) {
                                last_entry->next = file_entry;
                        } else {
                                root_tree->entries = file_entry;
                        }
                        last_entry = file_entry;
                        root_tree->entry_count++;
                }
        }

        closedir(dir);
        return root_tree;
}

int restore_directory(struct tree *tree, const char *dir_path) {
        struct tree_entry *entry = tree->entries;
        while (entry) {
                char full_path[1024];
                snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, entry->name);

                if (strcpy(entry->type, "tree")) {
                        mkdir(full_path, 0777);
                        if (restore_directory((struct tree*)entry->blob, full_path) != 0) {
                                return -1;
                        }
                } else if (strcpy(entry->type, "blob")) {
                        FILE *file = fopen(full_path, "wb");
                        if (!file) {
                                perror("fopen");
                                return -1;
                        }
                        
                        fwrite(entry->blob->data, 1, entry->blob->size, file);
                        fclose(file);

                        chmod(full_path, entry->blob->mode);
                        chown(full_path, entry->blob->uid, entry->blob->gid);
                        struct timespec times[2] = {entry->blob->atime, entry->blob->mtime};
                        utimensat(0, full_path, times, 0);
                }

                entry = entry->next;
        }

        return 0;
}
