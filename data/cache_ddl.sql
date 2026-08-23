CREATE TABLE IF NOT EXISTS dir (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    /* full path with slashes and with trailing slash; relative to configured root directory */
    name TEXT UNIQUE NOT NULL CHECK (name GLOB '*[^ ]/' AND name NOT GLOB '*//*' AND name NOT GLOB '*\*'),
    mtime INTEGER NOT NULL  /* last modified time of the directory, in seconds since epoch */
);
CREATE INDEX IF NOT EXISTS ix_dir_mtime ON dir(mtime);

CREATE TABLE IF NOT EXISTS file (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    dir_id INTEGER NOT NULL REFERENCES dir(id),
    /* file name with extension, without dir: */
    basename TEXT NOT NULL CHECK (basename GLOB '*[^ ]*' AND basename NOT GLOB '*[/\\]*'),
    mtime INTEGER NOT NULL,  /* last modified time of the file, in seconds since epoch */
    size INTEGER NOT NULL,  /* size of the file, in bytes */
    md5 TEXT DEFAULT NULL CHECK (md5 IS NULL OR (length(md5) = 32 AND md5 NOT GLOB '*[^0-9a-f]*')),
    checked INTEGER NOT NULL,  /* last check time of the file, in seconds since epoch */
    UNIQUE(dir_id, basename)
);

CREATE INDEX IF NOT EXISTS ix_file_mtime ON file(mtime);
CREATE INDEX IF NOT EXISTS ix_file_size ON file(size);

CREATE TABLE IF NOT EXISTS content (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    file_id INTEGER NOT NULL UNIQUE REFERENCES file(id) ON DELETE CASCADE,
    /* Seconds since epoch of when the photo was taken, extracted from metadata, used for sorting */
    /* If OffsetTimeOriginal is missing, assume GMT */
    /* Fall back to mtime if missing */
    taken INTEGER NOT NULL CHECK (taken BETWEEN strftime('%s', '1800-01-01') AND strftime('%s', '2250-12-31')),
    width INTEGER NOT NULL CHECK (width > 0),  /* Width of the photo in pixels, used for filtering & searching */
    height INTEGER NOT NULL CHECK (height > 0),  /* Height of the photo in pixels, used for filtering & searching */
    latitude REAL CHECK (latitude IS NULL OR latitude BETWEEN -90.0 AND 90.0),
    longitude REAL CHECK (longitude IS NULL OR longitude BETWEEN -180.0 AND 180.0),
    /* JSON object containing metadata extracted from the photo, see src/exiftool_response_schema.h */
    data JSON NOT NULL,
    CHECK ((latitude IS NULL) = (longitude IS NULL))
);
CREATE INDEX IF NOT EXISTS ix_content_taken ON content(taken);
CREATE INDEX IF NOT EXISTS ix_content_latitude ON content(latitude);
CREATE INDEX IF NOT EXISTS ix_content_longitude ON content(longitude);


CREATE TABLE IF NOT EXISTS fts_key (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    name TEXT NOT NULL UNIQUE CHECK (name IN (
        "DateOriginal",
        "YearMonthOriginal",
        "YearOriginal",
        "Creator",
        "Country",
        "State",
        "City",
        "Location",
        "Description",
        "Keyword",
        "Face",
        "Make",
        "Model"
    )),
    enabled INTEGER NOT NULL DEFAULT 1 CHECK (enabled IN (0, 1))
);

INSERT INTO fts_key (name)
VALUES
    ("DateOriginal"),
    ("YearOriginal"),
    ("YearMonthOriginal"),
    ("Creator"),
    ("Country"),
    ("State"),
    ("City"),
    ("Location"),
    ("Description"),
    ("Keyword"),
    ("Face"),
    ("Make"),
    ("Model")
ON CONFLICT(name) DO NOTHING;


CREATE TABLE IF NOT EXISTS token (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    /*
    Any kinda text, accented or not, might contain spaces but not leading/trailing/consecutive spaces,
    used for filtering & searching. Examples: "Eiffel Tower", "John Doe", "Paris", "2023-08-15", "14:00"
    */
    name TEXT NOT NULL UNIQUE CHECK (
        name GLOB '[^ ]*'  /* no leading space, at least 1 char */
        AND name GLOB '*[^ ]'  /* no trailing space */
        AND name NOT GLOB '*  *'  /* no consecutive spaces */
        AND instr(name, char(9)) = 0  /* no tab */
        AND instr(name, char(10)) = 0  /* no newline */
        AND instr(name, char(13)) = 0  /* no carriage return */
        AND instr(name, '(') = 0
        AND instr(name, ')') = 0
        AND instr(name, '|') = 0
        AND name NOT GLOB '-*'  /* no leading dash, reserved for search syntax */
    )
);
CREATE TABLE IF NOT EXISTS content_token (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    content_id INTEGER NOT NULL REFERENCES content(id) ON DELETE CASCADE,
    fts_key_id INTEGER NOT NULL REFERENCES fts_key(id),
    token_id INTEGER NOT NULL REFERENCES token(id)
);
CREATE UNIQUE INDEX IF NOT EXISTS uq_content_token ON content_token (
   token_id,
   content_id,
   fts_key_id
);

CREATE TABLE IF NOT EXISTS word (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    name TEXT NOT NULL UNIQUE CHECK (
        length(name) > 0
        AND name NOT GLOB '*[][ !"()*,:;<=>?{|}^`' || char(30) || '-' || char(31) || ']*'  /* no punctuation chars */
    ),
    simple TEXT NOT NULL CHECK (
        length(simple) > 0
        AND simple NOT GLOB '*[^0-9a-z+./-]*'
    )  /* lowercase unaccented ASCII + selected sigils */
);

CREATE INDEX IF NOT EXISTS ix_word_simple ON word(simple);
CREATE INDEX IF NOT EXISTS ix_word_name ON word(name);

CREATE TABLE IF NOT EXISTS token_word (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    token_id INTEGER NOT NULL REFERENCES token(id) ON DELETE CASCADE,
    word_id INTEGER NOT NULL REFERENCES word(id) ON DELETE CASCADE
);
CREATE UNIQUE INDEX IF NOT EXISTS uq_token_word ON token_word (
   token_id,
   word_id
);

CREATE INDEX IF NOT EXISTS ix_token_word ON token_word (
   word_id,
   token_id
);
