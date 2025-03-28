#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <utime.h>
#include <errno.h>
#include <zlib.h>
#include <openssl/sha.h>
#include "main.h"
#include "tree.h"
#include "delta.h"

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

        unsigned char *raw_data = malloc(st.st_size);
        if (!raw_data) {
                perror("malloc");
                fclose(file);
                free(blob);
                return NULL;
        }

        if (fread(raw_data, 1, st.st_size, file) != st.st_size) {
                perror("fread");
                free(raw_data);
                fclose(file);
                free(blob);
                return NULL;
        }
        fclose(file);

        blob->size = st.st_size;

        if (config.compress_files) {
                uLong compressed_size = compressBound(st.st_size);
                unsigned char *compressed_data = malloc(compressed_size);
                if (!compressed_data) {
                        perror("malloc");
                        free(raw_data);
                        free(blob);
                        return NULL;
                }

                if (compress(compressed_data, &compressed_size, raw_data, st.st_size) != Z_OK) {
                        free(compressed_data);
                        free(raw_data);
                        free(blob);
                        return NULL;
                }

                blob->data = compressed_data;
                blob->compressed_size = compressed_size;
                free(raw_data);
        } else {
                blob->data = raw_data;
                blob->compressed_size = st.st_size;
        }

        strcpy(blob->type, "blob");
        blob->mode = st.st_mode;
        blob->uid = st.st_uid;
        blob->gid = st.st_gid;
        blob->atime = st.st_atim;
        blob->mtime = st.st_mtim;
        blob->ctime = st.st_ctim;
        blob->link_target = NULL;

        return blob;
}

struct tree_entry *create_tree_entry(const char *name, struct blob *blob) 
{
        struct tree_entry *entry = malloc(sizeof(struct tree_entry));
        if (!entry) {
                perror("malloc");
                return NULL;
        }

        strncpy(entry->name, name, sizeof(entry->name) - 1);
        entry->name[sizeof(entry->name) - 1] = '\0';

        if (blob) {
                strcpy(entry->type, "blob");
                entry->blob = blob;
                SHA1(blob->data, blob->size, entry->hash);
        } else {
                strcpy(entry->type, "tree");
                entry->blob = NULL;
                memset(entry->hash, 0, sizeof(entry->hash));
        }

        snprintf(entry->mode, sizeof(entry->mode), "%06o", 
                blob ? blob->mode : S_IFDIR | 0755);
        entry->next = NULL;
        entry->subtree = NULL;
        entry->delta = NULL;

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
        tree->entry_count = entry ? 1 : 0;
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
                if (strcmp(entry->d_name, ".") == 0 || 
                    strcmp(entry->d_name, "..") == 0) {
                        continue;
                }

                char full_path[1024];
                snprintf(full_path, sizeof(full_path), "%s/%s", 
                        dir_path, entry->d_name);

                struct stat st;
                if (lstat(full_path, &st) < 0) {
                        perror("lstat");
                        continue;
                }

                struct tree_entry *new_entry = NULL;

                if (S_ISDIR(st.st_mode)) {
                        struct tree *subdir_tree = form_tree(full_path);
                        if (!subdir_tree) {
                                continue;
                        }
                        new_entry = create_tree_entry(entry->d_name, NULL);
                        if (!new_entry) {
                                free_tree(subdir_tree);
                                continue;
                        }
                        strcpy(new_entry->type, "tree");
                        new_entry->subtree = subdir_tree;
                } else if (S_ISREG(st.st_mode)) {
                        struct blob *file_blob = create_blob(full_path);
                        if (!file_blob) {
                                continue;
                        }
                        new_entry = create_tree_entry(entry->d_name, file_blob);
                        if (!new_entry) {
                                free(file_blob->data);
                                free(file_blob);
                                continue;
                        }
                }

                if (new_entry) {
                        if (last_entry) {
                                last_entry->next = new_entry;
                        } else {
                                root_tree->entries = new_entry;
                        }
                        last_entry = new_entry;
                        root_tree->entry_count++;
                }
        }

        closedir(dir);

        // Calculate tree hash
        FILE *tmp_file = tmpfile();
        if (!tmp_file) {
                perror("tmpfile");
                free_tree(root_tree);
                return NULL;
        }

        if (serialize_tree(tmp_file, root_tree) != 0) {
                fclose(tmp_file);
                free_tree(root_tree);
                return NULL;
        }

        fseek(tmp_file, 0, SEEK_END);
        size_t size = ftell(tmp_file);
        fseek(tmp_file, 0, SEEK_SET);

        unsigned char *buffer = malloc(size);
        if (!buffer) {
                perror("malloc");
                fclose(tmp_file);
                free_tree(root_tree);
                return NULL;
        }

        if (fread(buffer, 1, size, tmp_file) != size) {
                perror("fread");
                free(buffer);
                fclose(tmp_file);
                free_tree(root_tree);
                return NULL;
        }

        SHA1(buffer, size, root_tree->hash);

        free(buffer);
        fclose(tmp_file);

        return root_tree;
}

void free_tree_entry(struct tree_entry *entry) 
{
        if (!entry) return;

        if (entry->subtree) {
                free_tree(entry->subtree);
        }

        if (entry->blob) {
                free(entry->blob->data);
                free(entry->blob);
        }

        if (entry->delta) {
                free(entry->delta->added_data);
                free(entry->delta->deleted_data);
                free(entry->delta);
        }

        free(entry);
}

void free_tree_entries(struct tree_entry *entry) 
{
        while (entry) {
                struct tree_entry *next = entry->next;
                free_tree_entry(entry);
                entry = next;
        }
}

void free_tree(struct tree *t) 
{
        if (!t) return;
        free_tree_entries(t->entries);
        free(t);
}

int serialize_tree(FILE *out, struct tree *tree) 
{
        if (!tree || !out) return -1;

        if (fwrite(&tree->type, sizeof(tree->type), 1, out) != 1 ||
            fwrite(&tree->entry_count, sizeof(tree->entry_count), 1, out) != 1) {
                return -1;
        }

        struct tree_entry *entry = tree->entries;
        while (entry) {
                if (fwrite(entry->mode, sizeof(entry->mode), 1, out) != 1 ||
                    fwrite(&entry->type, sizeof(entry->type), 1, out) != 1 ||
                    fwrite(entry->name, sizeof(entry->name), 1, out) != 1 ||
                    fwrite(entry->hash, sizeof(entry->hash), 1, out) != 1) {
                        return -1;
                }

                if (strcmp(entry->type, "blob") == 0 && entry->blob) {
                        if (fwrite(&entry->blob->size, sizeof(entry->blob->size), 1, out) != 1 ||
                            fwrite(&entry->blob->compressed_size, sizeof(entry->blob->compressed_size), 1, out) != 1 ||
                            fwrite(entry->blob->data, 1, entry->blob->compressed_size, out) != entry->blob->compressed_size ||
                            fwrite(&entry->blob->mode, sizeof(entry->blob->mode), 1, out) != 1 ||
                            fwrite(&entry->blob->uid, sizeof(entry->blob->uid), 1, out) != 1 ||
                            fwrite(&entry->blob->gid, sizeof(entry->blob->gid), 1, out) != 1 ||
                            fwrite(&entry->blob->atime, sizeof(entry->blob->atime), 1, out) != 1 ||
                            fwrite(&entry->blob->mtime, sizeof(entry->blob->mtime), 1, out) != 1 ||
                            fwrite(&entry->blob->ctime, sizeof(entry->blob->ctime), 1, out) != 1) {
                                return -1;
                        }
                }
                else if (strcmp(entry->type, "tree") == 0 && entry->subtree) {
                        if (serialize_tree(out, entry->subtree) != 0) {
                                return -1;
                        }
                }
                entry = entry->next;
        }
        return 0;
}

int deserialize_tree(FILE *in, struct tree **tree) 
{
        if (!in || !tree) return -1;

        *tree = malloc(sizeof(struct tree));
        if (!*tree) return -1;

        if (fread(&(*tree)->type, sizeof((*tree)->type), 1, in) != 1 ||
            fread(&(*tree)->entry_count, sizeof((*tree)->entry_count), 1, in) != 1) {
                free(*tree);
                *tree = NULL;
                return -1;
        }

        (*tree)->entries = NULL;
        struct tree_entry **last_entry = &(*tree)->entries;

        for (size_t i = 0; i < (*tree)->entry_count; i++) {
                struct tree_entry *entry = malloc(sizeof(struct tree_entry));
                if (!entry) {
                        free_tree(*tree);
                        *tree = NULL;
                        return -1;
                }
                entry->next = NULL;
                entry->blob = NULL;
                entry->subtree = NULL;
                entry->delta = NULL;

                if (fread(entry->mode, sizeof(entry->mode), 1, in) != 1 ||
                    fread(&entry->type, sizeof(entry->type), 1, in) != 1 ||
                    fread(entry->name, sizeof(entry->name), 1, in) != 1 ||
                    fread(entry->hash, sizeof(entry->hash), 1, in) != 1) {
                        free(entry);
                        free_tree(*tree);
                        *tree = NULL;
                        return -1;
                }

                if (strcmp(entry->type, "blob") == 0) {
                        entry->blob = malloc(sizeof(struct blob));
                        if (!entry->blob) {
                                free(entry);
                                free_tree(*tree);
                                *tree = NULL;
                                return -1;
                        }

                        if (fread(&entry->blob->size, sizeof(entry->blob->size), 1, in) != 1 ||
                            fread(&entry->blob->compressed_size, sizeof(entry->blob->compressed_size), 1, in) != 1) {
                                free(entry->blob);
                                free(entry);
                                free_tree(*tree);
                                *tree = NULL;
                                return -1;
                        }

                        entry->blob->data = malloc(entry->blob->compressed_size);
                        if (!entry->blob->data) {
                                free(entry->blob);
                                free(entry);
                                free_tree(*tree);
                                *tree = NULL;
                                return -1;
                        }

                        if (fread(entry->blob->data, 1, entry->blob->compressed_size, in) != entry->blob->compressed_size ||
                            fread(&entry->blob->mode, sizeof(entry->blob->mode), 1, in) != 1 ||
                            fread(&entry->blob->uid, sizeof(entry->blob->uid), 1, in) != 1 ||
                            fread(&entry->blob->gid, sizeof(entry->blob->gid), 1, in) != 1 ||
                            fread(&entry->blob->atime, sizeof(entry->blob->atime), 1, in) != 1 ||
                            fread(&entry->blob->mtime, sizeof(entry->blob->mtime), 1, in) != 1 ||
                            fread(&entry->blob->ctime, sizeof(entry->blob->ctime), 1, in) != 1) {
                                free(entry->blob->data);
                                free(entry->blob);
                                free(entry);
                                free_tree(*tree);
                                *tree = NULL;
                                return -1;
                        }
                }
                else if (strcmp(entry->type, "tree") == 0) {
                        if (deserialize_tree(in, &entry->subtree) != 0) {
                                free(entry);
                                free_tree(*tree);
                                *tree = NULL;
                                return -1;
                        }
                }

                *last_entry = entry;
                last_entry = &entry->next;
        }
        return 0;
}

int restore_directory(struct tree *tree, const char *dir_path) 
{
        if (!tree || !dir_path) {
                fprintf(stderr, "Invalid arguments to restore_directory\n");
                return -1;
        }

        if (mkdir(dir_path, 0777) < 0 && errno != EEXIST) {
                perror("mkdir");
                return -1;
        }

        struct tree_entry *entry = tree->entries;
        while (entry) {
                char full_path[1024];
                snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, entry->name);

                if (strcmp(entry->type, "tree") == 0) {
                        if (entry->subtree) {
                                if (restore_directory(entry->subtree, full_path) != 0) {
                                        return -1;
                                }
                        }
                } else if (strcmp(entry->type, "blob") == 0) {
                        if (!entry->blob) {
                                fprintf(stderr, "Invalid blob for entry %s\n", entry->name);
                                return -1;
                        }

                        unsigned char *write_data;
                        size_t write_size;

                        if (config.compress_files && entry->blob->size != entry->blob->compressed_size) {
                                write_data = malloc(entry->blob->size);
                                if (!write_data) return -1;

                                uLong uncomp_size = entry->blob->size;
                                if (uncompress(write_data, &uncomp_size, entry->blob->data, 
                                             entry->blob->compressed_size) != Z_OK) {
                                        free(write_data);
                                        return -1;
                                }
                                write_size = uncomp_size;
                        } else {
                                write_data = entry->blob->data;
                                write_size = entry->blob->size;
                        }

                        FILE *file = fopen(full_path, "wb");
                        if (!file) {
                                if (config.compress_files && write_data != entry->blob->data) 
                                        free(write_data);
                                perror("fopen");
                                return -1;
                        }
                        
                        if (fwrite(write_data, 1, write_size, file) != write_size) {
                                if (config.compress_files && write_data != entry->blob->data) 
                                        free(write_data);
                                perror("fwrite");
                                fclose(file);
                                return -1;
                        }

                        if (config.compress_files && write_data != entry->blob->data) {
                                free(write_data);
                        }
                        fclose(file);

                        if (chmod(full_path, entry->blob->mode) < 0) {
                                perror("chmod");
                                return -1;
                        }
                        
                        if (chown(full_path, entry->blob->uid, entry->blob->gid) < 0) {
                                perror("chown");
                        }

                        struct timespec times[2] = {entry->blob->atime, entry->blob->mtime};
                        if (utimensat(0, full_path, times, 0) < 0) {
                                perror("utimensat");
                                return -1;
                        }
                }
                entry = entry->next;
        }
        return 0;
}

void print_indentation(int depth)
{
        for (int i = 0; i < depth && i < 100; i++) {
                printf("  ");
        }
}

int print_tree_entry(struct tree_entry *entry, int depth, int *total_entries) 
{
        if (!entry) return 0;
        if (depth >= 100) {
                printf("Warning: Maximum depth reached. Tree may be corrupted or contain cycles.\n");
                return -1;
        }

        if (total_entries && *total_entries > 1000000) {  // Arbitrary large number to prevent infinite loops
                        printf("Warning: Too many entries. Tree may contain cycles.\n");
                return -1;
        }

        // Print indentation based on depth
        print_indentation(depth);

        // Print entry information
        printf("%s %s %s ", entry->mode, entry->type, entry->name);

        // Print hash in hex format
        for (int i = 0; i < SHA_DIGEST_LENGTH; i++) {
                printf("%02x", entry->hash[i]);
        }
        printf("\n");

        // If this entry has a blob, print its details
        if (entry->blob) {
                print_indentation(depth + 1);
                printf("Blob: type=%s size=%zu compressed=%zu\n", 
                       entry->blob->type,
                       entry->blob->size,
                       entry->blob->compressed_size);

                if (entry->blob->link_target) {
                        print_indentation(depth + 1);
                        printf("-> %s\n", entry->blob->link_target);
                }
        }

        if (entry->subtree) {
                print_indentation(depth + 1);
                printf("Subtree:\n");
                if (print_tree(entry->subtree, depth + 2, total_entries) != 0) {
                        return -1;
                }
        }

        if (total_entries) {
                (*total_entries)++;
        }

        return print_tree_entry(entry->next, depth, total_entries);
}

int print_tree(struct tree *tree, int depth, int *total_entries) 
{
        if (!tree) return 0;
                if (depth >= 100) {
                        printf("Warning: Maximum depth reached. Tree may be corrupted or contain cycles.\n");
                        return -1;
                }

                print_indentation(depth);
                printf("Tree: type=%s entries=%zu hash=", tree->type, tree->entry_count);

                // Print tree hash
                for (int i = 0; i < SHA_DIGEST_LENGTH; i++) {
                        printf("%02x", tree->hash[i]);
                }
                printf("\n");

                if (tree->entries) {
                return print_tree_entry(tree->entries, depth + 1, total_entries);
        }

        return 0;
}

int print_tree_structure(struct tree *root) 
{
        printf("Tree Structure:\n");
        int total_entries = 0;
        return print_tree(root, 0, &total_entries);
}
