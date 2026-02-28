#ifndef STRATA_SCHEMA_H
#define STRATA_SCHEMA_H

/* SQL statements for schema creation - standard SQL compatible with SQLite and PG */
#define STRATA_SCHEMA_SQL \
    "CREATE TABLE IF NOT EXISTS repos (" \
    "  repo_id    TEXT PRIMARY KEY," \
    "  name       TEXT NOT NULL," \
    "  created_at TEXT NOT NULL" \
    ");" \
    "CREATE TABLE IF NOT EXISTS artifacts (" \
    "  artifact_id   TEXT PRIMARY KEY," \
    "  repo_id       TEXT NOT NULL REFERENCES repos(repo_id)," \
    "  content       BLOB NOT NULL," \
    "  artifact_type TEXT NOT NULL," \
    "  author        TEXT NOT NULL," \
    "  created_at    TEXT NOT NULL," \
    "  parent_id     TEXT" \
    ");" \
    "CREATE TABLE IF NOT EXISTS artifact_roles (" \
    "  artifact_id TEXT NOT NULL REFERENCES artifacts(artifact_id)," \
    "  role_name   TEXT NOT NULL," \
    "  PRIMARY KEY (artifact_id, role_name)" \
    ");" \
    "CREATE TABLE IF NOT EXISTS role_assignments (" \
    "  entity_id  TEXT NOT NULL," \
    "  role_name  TEXT NOT NULL," \
    "  repo_id    TEXT NOT NULL REFERENCES repos(repo_id)," \
    "  granted_at TEXT NOT NULL," \
    "  expires_at TEXT," \
    "  PRIMARY KEY (entity_id, role_name, repo_id)" \
    ");"

#define STRATA_INDEX_SQL \
    "CREATE INDEX IF NOT EXISTS idx_artifact_roles_role ON artifact_roles(role_name);" \
    "CREATE INDEX IF NOT EXISTS idx_artifacts_repo ON artifacts(repo_id);" \
    "CREATE INDEX IF NOT EXISTS idx_role_assignments_entity ON role_assignments(entity_id, repo_id);"

#endif
