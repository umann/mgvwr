CREATE TABLE IF NOT EXISTS dir (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    -- full path with slashes and with trailing slash; relative to configured root directory
    name TEXT UNIQUE NOT NULL CHECK (name GLOB '*[^ ]/' AND name NOT GLOB '*//*' AND name NOT GLOB '*\*'),
    mtime INTEGER NOT NULL -- last modified time of the directory, in seconds since epoch
);
CREATE INDEX IF NOT EXISTS ix_dir_mtime ON dir(mtime);

CREATE TABLE IF NOT EXISTS file (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    dir_id INTEGER NOT NULL REFERENCES dir(id),
    -- file name with extension, without dir:
    basename TEXT NOT NULL CHECK (basename GLOB '*[^ ]*' AND basename NOT GLOB '*[/\\]*'),
    mtime INTEGER NOT NULL, -- last modified time of the file, in seconds since epoch
    size INTEGER NOT NULL, -- size of the file, in bytes
    md5 TEXT DEFAULT NULL CHECK (md5 IS NULL OR (length(md5) = 32 AND md5 NOT GLOB '*[^0-9a-f]*')),
    checked INTEGER NOT NULL, -- last check time of the file, in seconds since epoch
    UNIQUE(dir_id, basename)
);

CREATE INDEX IF NOT EXISTS ix_file_mtime ON file(mtime);
CREATE INDEX IF NOT EXISTS ix_file_size ON file(size);

CREATE TABLE IF NOT EXISTS content (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    file_id INTEGER NOT NULL UNIQUE REFERENCES file(id) ON DELETE CASCADE,
    -- Seconds since epoch of when the photo was taken, extracted from metadata, used for sorting
    -- If OffsetTimeOriginal is missing, assume GMT
    -- Fall back to mtime if missing
    taken INTEGER NOT NULL CHECK (taken BETWEEN strftime('%s', '1800-01-01') AND strftime('%s', '2250-12-31')),
    width INTEGER NOT NULL CHECK (width > 0),  -- Width of the photo in pixels, used for filtering & searching
    height INTEGER NOT NULL CHECK (height > 0), -- Height of the photo in pixels, used for filtering & searching
    latitude REAL CHECK (latitude IS NULL OR latitude BETWEEN -90.0 AND 90.0),
    longitude REAL CHECK (longitude IS NULL OR longitude BETWEEN -180.0 AND 180.0),
    -- JSON object containing metadata extracted from the photo, see src/exiftool_response_schema.h
    data JSON NOT NULL,
    CHECK ((latitude IS NULL) = (longitude IS NULL))
);
CREATE INDEX IF NOT EXISTS ix_content_taken ON content(taken);
CREATE INDEX IF NOT EXISTS ix_content_latitude ON content(latitude);
CREATE INDEX IF NOT EXISTS ix_content_longitude ON content(longitude);