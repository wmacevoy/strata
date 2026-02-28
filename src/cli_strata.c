/*
 * strata CLI — village builder.
 *
 * Direct SQLite access for trusted admin operations:
 *   init, repo create, role assign/revoke,
 *   msg post/list/get, blob put/get/find/tag/untag/tags.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include "strata/store.h"
#include "strata/context.h"
#include "strata/blob.h"

/* ------------------------------------------------------------------ */
/*  Options                                                            */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *db_path;
    const char *entity;
    const char *roles_csv;
    const char *tags_csv;
    const char *type_filter;
    const char *file_path;
    int plain;
    int argc;
    char **argv;
} cli_opts;

/* ------------------------------------------------------------------ */
/*  CSV parsing                                                        */
/* ------------------------------------------------------------------ */

static int parse_csv(const char *csv, char **out, int max) {
    if (!csv || !csv[0]) return 0;
    char *copy = strdup(csv);
    int count = 0;
    char *tok = strtok(copy, ",");
    while (tok && count < max) {
        out[count++] = strdup(tok);
        tok = strtok(NULL, ",");
    }
    free(copy);
    return count;
}

static void free_csv(char **arr, int count) {
    for (int i = 0; i < count; i++) free(arr[i]);
}

/* ------------------------------------------------------------------ */
/*  Artifact list callback                                             */
/* ------------------------------------------------------------------ */

static int plain_mode = 0;
static int list_count = 0;

static int artifact_list_cb(const strata_artifact *a, void *userdata) {
    (void)userdata;
    if (plain_mode) {
        printf("%-64s  %-12s  %-16s  %s  %.*s\n",
            a->artifact_id, a->artifact_type, a->author,
            a->created_at, (int)a->content_len, a->content);
    } else {
        if (list_count > 0) printf(",\n");
        printf("  {\"id\":\"%s\",\"type\":\"%s\",\"author\":\"%s\","
               "\"created_at\":\"%s\",\"content\":\"%.*s\"}",
            a->artifact_id, a->artifact_type, a->author,
            a->created_at, (int)a->content_len, a->content);
    }
    list_count++;
    return 1;
}

static int blob_list_cb(const strata_blob *b, void *userdata) {
    (void)userdata;
    if (plain_mode) {
        printf("%-64s  %-16s  %s  %.*s\n",
            b->blob_id, b->author,
            b->created_at, (int)b->content_len, b->content);
    } else {
        if (list_count > 0) printf(",\n");
        printf("  {\"id\":\"%s\",\"author\":\"%s\","
               "\"created_at\":\"%s\",\"content\":\"%.*s\"}",
            b->blob_id, b->author,
            b->created_at, (int)b->content_len, b->content);
    }
    list_count++;
    return 1;
}

/* ------------------------------------------------------------------ */
/*  Commands                                                           */
/* ------------------------------------------------------------------ */

static int cmd_init(strata_store *store, cli_opts *opts) {
    (void)opts;
    strata_store_init(store);
    if (opts->plain)
        printf("initialized\n");
    else
        printf("{\"ok\":true}\n");
    return 0;
}

static int cmd_repo_create(strata_store *store, cli_opts *opts) {
    if (opts->argc < 2) {
        fprintf(stderr, "usage: strata --db <path> repo create <repo_id> <name>\n");
        return 1;
    }
    const char *repo_id = opts->argv[0];
    const char *name = opts->argv[1];
    int rc = strata_repo_create(store, repo_id, name);
    if (rc == 0) {
        if (opts->plain)
            printf("repo '%s' created\n", repo_id);
        else
            printf("{\"ok\":true,\"repo\":\"%s\"}\n", repo_id);
    } else {
        fprintf(stderr, "failed to create repo '%s'\n", repo_id);
        return 1;
    }
    return 0;
}

static int cmd_role_assign(strata_store *store, cli_opts *opts) {
    if (opts->argc < 3) {
        fprintf(stderr, "usage: strata --db <path> role assign <entity> <role> <repo_id>\n");
        return 1;
    }
    int rc = strata_role_assign(store, opts->argv[0], opts->argv[1], opts->argv[2]);
    if (rc == 0) {
        if (opts->plain)
            printf("assigned '%s' role '%s' on '%s'\n", opts->argv[0], opts->argv[1], opts->argv[2]);
        else
            printf("{\"ok\":true}\n");
    } else {
        fprintf(stderr, "role assign failed\n");
        return 1;
    }
    return 0;
}

static int cmd_role_revoke(strata_store *store, cli_opts *opts) {
    if (opts->argc < 3) {
        fprintf(stderr, "usage: strata --db <path> role revoke <entity> <role> <repo_id>\n");
        return 1;
    }
    int rc = strata_role_revoke(store, opts->argv[0], opts->argv[1], opts->argv[2]);
    if (rc == 0) {
        if (opts->plain)
            printf("revoked '%s' role '%s' on '%s'\n", opts->argv[0], opts->argv[1], opts->argv[2]);
        else
            printf("{\"ok\":true}\n");
    } else {
        fprintf(stderr, "role revoke failed\n");
        return 1;
    }
    return 0;
}

static int cmd_msg_post(strata_store *store, cli_opts *opts) {
    if (!opts->entity) { fprintf(stderr, "--entity required\n"); return 1; }
    if (opts->argc < 3) {
        fprintf(stderr, "usage: strata --db <path> --entity <id> msg post <repo> <type> <content> --roles r1,r2\n");
        return 1;
    }
    const char *repo = opts->argv[0];
    const char *type = opts->argv[1];
    const char *content = opts->argv[2];

    char *roles[16];
    int nroles = parse_csv(opts->roles_csv, roles, 16);
    if (nroles == 0) { fprintf(stderr, "--roles required\n"); return 1; }

    strata_ctx *ctx = strata_ctx_create(opts->entity);
    char id[65];
    int rc = strata_artifact_put(store, ctx, repo, type,
                                  content, strlen(content),
                                  (const char **)roles, nroles, id);
    strata_ctx_free(ctx);
    free_csv(roles, nroles);

    if (rc == 0) {
        if (opts->plain)
            printf("%s\n", id);
        else
            printf("{\"ok\":true,\"id\":\"%s\"}\n", id);
    } else {
        fprintf(stderr, "put failed\n");
        return 1;
    }
    return 0;
}

static int cmd_msg_list(strata_store *store, cli_opts *opts) {
    if (!opts->entity) { fprintf(stderr, "--entity required\n"); return 1; }
    if (opts->argc < 1) {
        fprintf(stderr, "usage: strata --db <path> --entity <id> msg list <repo> [--type <type>]\n");
        return 1;
    }
    const char *repo = opts->argv[0];
    const char *type = opts->type_filter;

    strata_ctx *ctx = strata_ctx_create(opts->entity);
    plain_mode = opts->plain;
    list_count = 0;

    if (opts->plain) {
        printf("%-64s  %-12s  %-16s  %-24s  %s\n", "ID", "TYPE", "AUTHOR", "CREATED", "CONTENT");
    } else {
        printf("[");
    }

    strata_artifact_list(store, ctx, repo, type && type[0] ? type : NULL, artifact_list_cb, NULL);

    if (!opts->plain) printf("\n]\n");
    strata_ctx_free(ctx);
    return 0;
}

static int cmd_msg_get(strata_store *store, cli_opts *opts) {
    if (!opts->entity) { fprintf(stderr, "--entity required\n"); return 1; }
    if (opts->argc < 1) {
        fprintf(stderr, "usage: strata --db <path> --entity <id> msg get <artifact_id>\n");
        return 1;
    }

    strata_ctx *ctx = strata_ctx_create(opts->entity);
    strata_artifact out;
    if (strata_artifact_get(store, ctx, opts->argv[0], &out) == 0) {
        if (opts->plain) {
            printf("ID:      %s\nRepo:    %s\nType:    %s\nAuthor:  %s\nCreated: %s\nContent: %.*s\n",
                out.artifact_id, out.repo_id, out.artifact_type,
                out.author, out.created_at, (int)out.content_len, out.content);
        } else {
            printf("{\"ok\":true,\"id\":\"%s\",\"repo\":\"%s\",\"type\":\"%s\","
                   "\"author\":\"%s\",\"created_at\":\"%s\",\"content\":\"%.*s\"}\n",
                out.artifact_id, out.repo_id, out.artifact_type,
                out.author, out.created_at, (int)out.content_len, out.content);
        }
        strata_artifact_cleanup(&out);
    } else {
        fprintf(stderr, "not found or no access\n");
        strata_ctx_free(ctx);
        return 1;
    }
    strata_ctx_free(ctx);
    return 0;
}

static int cmd_blob_put(strata_store *store, cli_opts *opts) {
    if (!opts->entity) { fprintf(stderr, "--entity required\n"); return 1; }

    const char *content = NULL;
    char *file_content = NULL;
    size_t content_len = 0;

    if (opts->file_path) {
        FILE *f = fopen(opts->file_path, "rb");
        if (!f) { fprintf(stderr, "cannot open file: %s\n", opts->file_path); return 1; }
        fseek(f, 0, SEEK_END);
        content_len = (size_t)ftell(f);
        fseek(f, 0, SEEK_SET);
        file_content = malloc(content_len);
        fread(file_content, 1, content_len, f);
        fclose(f);
        content = file_content;
    } else if (opts->argc >= 1) {
        content = opts->argv[0];
        content_len = strlen(content);
    } else {
        fprintf(stderr, "usage: strata --db <path> --entity <id> blob put <content> --tags t1,t2 --roles r1,r2\n");
        return 1;
    }

    char *tags[16], *roles[16];
    int ntags = parse_csv(opts->tags_csv, tags, 16);
    int nroles = parse_csv(opts->roles_csv, roles, 16);
    if (nroles == 0) { free(file_content); free_csv(tags, ntags); fprintf(stderr, "--roles required\n"); return 1; }

    strata_ctx *ctx = strata_ctx_create(opts->entity);
    char id[65];
    int rc = strata_blob_put(store, ctx, content, content_len,
                              (const char **)tags, ntags,
                              (const char **)roles, nroles, id);
    strata_ctx_free(ctx);
    free_csv(tags, ntags);
    free_csv(roles, nroles);
    free(file_content);

    if (rc == 0) {
        if (opts->plain)
            printf("%s\n", id);
        else
            printf("{\"ok\":true,\"id\":\"%s\"}\n", id);
    } else {
        fprintf(stderr, "blob put failed\n");
        return 1;
    }
    return 0;
}

static int cmd_blob_get(strata_store *store, cli_opts *opts) {
    if (!opts->entity) { fprintf(stderr, "--entity required\n"); return 1; }
    if (opts->argc < 1) {
        fprintf(stderr, "usage: strata --db <path> --entity <id> blob get <blob_id>\n");
        return 1;
    }

    strata_ctx *ctx = strata_ctx_create(opts->entity);
    strata_blob out;
    if (strata_blob_get(store, ctx, opts->argv[0], &out) == 0) {
        if (opts->plain) {
            printf("ID:      %s\nAuthor:  %s\nCreated: %s\nContent: %.*s\n",
                out.blob_id, out.author, out.created_at,
                (int)out.content_len, out.content);
        } else {
            printf("{\"ok\":true,\"id\":\"%s\",\"author\":\"%s\","
                   "\"created_at\":\"%s\",\"content\":\"%.*s\"}\n",
                out.blob_id, out.author, out.created_at,
                (int)out.content_len, out.content);
        }
        strata_blob_cleanup(&out);
    } else {
        fprintf(stderr, "not found or no access\n");
        strata_ctx_free(ctx);
        return 1;
    }
    strata_ctx_free(ctx);
    return 0;
}

static int cmd_blob_find(strata_store *store, cli_opts *opts) {
    if (!opts->entity) { fprintf(stderr, "--entity required\n"); return 1; }

    char *tags[16];
    int ntags = parse_csv(opts->tags_csv, tags, 16);

    strata_ctx *ctx = strata_ctx_create(opts->entity);
    plain_mode = opts->plain;
    list_count = 0;

    if (opts->plain) {
        printf("%-64s  %-16s  %-24s  %s\n", "ID", "AUTHOR", "CREATED", "CONTENT");
    } else {
        printf("[");
    }

    strata_blob_find(store, ctx, (const char **)tags, ntags, blob_list_cb, NULL);

    if (!opts->plain) printf("\n]\n");
    strata_ctx_free(ctx);
    free_csv(tags, ntags);
    return 0;
}

static int cmd_blob_tag(strata_store *store, cli_opts *opts) {
    if (opts->argc < 2) {
        fprintf(stderr, "usage: strata --db <path> blob tag <blob_id> <tag>\n");
        return 1;
    }
    int rc = strata_blob_tag(store, opts->argv[0], opts->argv[1]);
    if (rc == 0) {
        if (opts->plain) printf("tagged\n"); else printf("{\"ok\":true}\n");
    } else {
        fprintf(stderr, "blob tag failed\n"); return 1;
    }
    return 0;
}

static int cmd_blob_untag(strata_store *store, cli_opts *opts) {
    if (opts->argc < 2) {
        fprintf(stderr, "usage: strata --db <path> blob untag <blob_id> <tag>\n");
        return 1;
    }
    int rc = strata_blob_untag(store, opts->argv[0], opts->argv[1]);
    if (rc == 0) {
        if (opts->plain) printf("untagged\n"); else printf("{\"ok\":true}\n");
    } else {
        fprintf(stderr, "blob untag failed\n"); return 1;
    }
    return 0;
}

static int cmd_blob_tags(strata_store *store, cli_opts *opts) {
    if (opts->argc < 1) {
        fprintf(stderr, "usage: strata --db <path> blob tags <blob_id>\n");
        return 1;
    }
    char **tags = NULL;
    int count = 0;
    if (strata_blob_tags(store, opts->argv[0], &tags, &count) == 0) {
        if (opts->plain) {
            for (int i = 0; i < count; i++) printf("%s\n", tags[i]);
        } else {
            printf("[");
            for (int i = 0; i < count; i++)
                printf("%s\"%s\"", i > 0 ? "," : "", tags[i]);
            printf("]\n");
        }
        strata_blob_free_tags(tags, count);
    } else {
        fprintf(stderr, "blob tags failed\n"); return 1;
    }
    return 0;
}

static int cmd_privilege_grant(strata_store *store, cli_opts *opts) {
    if (opts->argc < 2) {
        fprintf(stderr, "usage: strata --db <path> privilege grant <entity> <privilege>\n"
                        "  privileges: core, parent, vocation\n");
        return 1;
    }
    int rc = strata_role_assign(store, opts->argv[0], opts->argv[1], "_system");
    if (rc == 0) {
        if (opts->plain)
            printf("granted '%s' privilege '%s'\n", opts->argv[0], opts->argv[1]);
        else
            printf("{\"ok\":true,\"entity\":\"%s\",\"privilege\":\"%s\"}\n",
                   opts->argv[0], opts->argv[1]);
    } else {
        fprintf(stderr, "privilege grant failed\n");
        return 1;
    }
    return 0;
}

static int cmd_privilege_revoke(strata_store *store, cli_opts *opts) {
    if (opts->argc < 2) {
        fprintf(stderr, "usage: strata --db <path> privilege revoke <entity> <privilege>\n");
        return 1;
    }
    int rc = strata_role_revoke(store, opts->argv[0], opts->argv[1], "_system");
    if (rc == 0) {
        if (opts->plain)
            printf("revoked '%s' privilege '%s'\n", opts->argv[0], opts->argv[1]);
        else
            printf("{\"ok\":true}\n");
    } else {
        fprintf(stderr, "privilege revoke failed\n");
        return 1;
    }
    return 0;
}

static int cmd_privilege_check(strata_store *store, cli_opts *opts) {
    if (opts->argc < 2) {
        fprintf(stderr, "usage: strata --db <path> privilege check <entity> <privilege>\n");
        return 1;
    }
    int has = strata_has_privilege(store, opts->argv[0], opts->argv[1]);
    if (opts->plain)
        printf("%s\n", has ? "yes" : "no");
    else
        printf("{\"ok\":true,\"entity\":\"%s\",\"privilege\":\"%s\",\"has\":%s}\n",
               opts->argv[0], opts->argv[1], has ? "true" : "false");
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Usage / main                                                       */
/* ------------------------------------------------------------------ */

static void usage(void) {
    fprintf(stderr,
        "usage: strata --db <path> [--entity <id>] [--plain] <command> [args...]\n"
        "\n"
        "Admin:\n"
        "  init                              Initialize schema\n"
        "  repo create <id> <name>           Create repository\n"
        "  role assign <entity> <role> <repo> Assign role\n"
        "  role revoke <entity> <role> <repo> Revoke role\n"
        "\n"
        "Privileges:\n"
        "  privilege grant <entity> <priv>   Grant privilege (core/parent/vocation)\n"
        "  privilege revoke <entity> <priv>  Revoke privilege\n"
        "  privilege check <entity> <priv>   Check if entity has privilege\n"
        "\n"
        "Messages (require --entity):\n"
        "  msg post <repo> <type> <content> --roles r1,r2\n"
        "  msg list <repo> [--type <type>]\n"
        "  msg get <artifact_id>\n"
        "\n"
        "Blobs (require --entity for put/get/find):\n"
        "  blob put <content> --tags t1,t2 --roles r1,r2\n"
        "  blob put --file <path> --tags t1,t2 --roles r1,r2\n"
        "  blob get <blob_id>\n"
        "  blob find [--tags t1,t2]\n"
        "  blob tag <blob_id> <tag>\n"
        "  blob untag <blob_id> <tag>\n"
        "  blob tags <blob_id>\n"
    );
}

int main(int argc, char **argv) {
    cli_opts opts = {0};

    static struct option long_opts[] = {
        {"db",     required_argument, NULL, 'd'},
        {"entity", required_argument, NULL, 'e'},
        {"plain",  no_argument,       NULL, 'p'},
        {"roles",  required_argument, NULL, 'r'},
        {"tags",   required_argument, NULL, 't'},
        {"type",   required_argument, NULL, 'T'},
        {"file",   required_argument, NULL, 'f'},
        {"help",   no_argument,       NULL, 'h'},
        {NULL, 0, NULL, 0}
    };

    int c;
    while ((c = getopt_long(argc, argv, "d:e:pr:t:T:f:h", long_opts, NULL)) != -1) {
        switch (c) {
            case 'd': opts.db_path = optarg; break;
            case 'e': opts.entity = optarg; break;
            case 'p': opts.plain = 1; break;
            case 'r': opts.roles_csv = optarg; break;
            case 't': opts.tags_csv = optarg; break;
            case 'T': opts.type_filter = optarg; break;
            case 'f': opts.file_path = optarg; break;
            case 'h': usage(); return 0;
            default: usage(); return 1;
        }
    }

    opts.argc = argc - optind;
    opts.argv = argv + optind;

    if (!opts.db_path) { fprintf(stderr, "--db required\n"); usage(); return 1; }
    if (opts.argc < 1) { usage(); return 1; }

    /* Open store */
    strata_store *store = strata_store_open_sqlite(opts.db_path);
    if (!store) { fprintf(stderr, "failed to open database: %s\n", opts.db_path); return 1; }

    /* Parse compound command */
    const char *cmd = opts.argv[0];
    const char *subcmd = opts.argc > 1 ? opts.argv[1] : NULL;
    char fullcmd[64];
    int consumed = 1;

    if (subcmd && (strcmp(cmd, "repo") == 0 || strcmp(cmd, "role") == 0 ||
                   strcmp(cmd, "msg") == 0 || strcmp(cmd, "blob") == 0 ||
                   strcmp(cmd, "privilege") == 0)) {
        snprintf(fullcmd, sizeof(fullcmd), "%s_%s", cmd, subcmd);
        consumed = 2;
    } else {
        strncpy(fullcmd, cmd, sizeof(fullcmd) - 1);
        fullcmd[sizeof(fullcmd) - 1] = '\0';
    }
    opts.argv += consumed;
    opts.argc -= consumed;

    /* Dispatch */
    int rc = 1;
    if (strcmp(fullcmd, "init") == 0)             rc = cmd_init(store, &opts);
    else if (strcmp(fullcmd, "repo_create") == 0) rc = cmd_repo_create(store, &opts);
    else if (strcmp(fullcmd, "role_assign") == 0)  rc = cmd_role_assign(store, &opts);
    else if (strcmp(fullcmd, "role_revoke") == 0)  rc = cmd_role_revoke(store, &opts);
    else if (strcmp(fullcmd, "msg_post") == 0)     rc = cmd_msg_post(store, &opts);
    else if (strcmp(fullcmd, "msg_list") == 0)     rc = cmd_msg_list(store, &opts);
    else if (strcmp(fullcmd, "msg_get") == 0)      rc = cmd_msg_get(store, &opts);
    else if (strcmp(fullcmd, "blob_put") == 0)     rc = cmd_blob_put(store, &opts);
    else if (strcmp(fullcmd, "blob_get") == 0)     rc = cmd_blob_get(store, &opts);
    else if (strcmp(fullcmd, "blob_find") == 0)    rc = cmd_blob_find(store, &opts);
    else if (strcmp(fullcmd, "blob_tag") == 0)     rc = cmd_blob_tag(store, &opts);
    else if (strcmp(fullcmd, "blob_untag") == 0)   rc = cmd_blob_untag(store, &opts);
    else if (strcmp(fullcmd, "blob_tags") == 0)    rc = cmd_blob_tags(store, &opts);
    else if (strcmp(fullcmd, "privilege_grant") == 0)  rc = cmd_privilege_grant(store, &opts);
    else if (strcmp(fullcmd, "privilege_revoke") == 0) rc = cmd_privilege_revoke(store, &opts);
    else if (strcmp(fullcmd, "privilege_check") == 0)  rc = cmd_privilege_check(store, &opts);
    else { fprintf(stderr, "unknown command: %s\n", fullcmd); usage(); rc = 1; }

    strata_store_close(store);
    return rc;
}
