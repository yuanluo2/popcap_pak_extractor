/*
    @author yuanluo2
    @brief popcap's .pak file extractor written in ANSI C, no 3rd parties, only works for windows platform.
*/
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct ArenaBlockHeader  ArenaBlockHeader;
typedef struct ArenaAllocator    ArenaAllocator;

typedef struct FileAttr      FileAttr;
typedef struct FileAttrList  FileAttrList;
typedef struct PakHeader     PakHeader;
typedef struct Resource      Resource;

#define BYTES_OF_MAGIC       4
#define BYTES_OF_VERSION     4
#define BYTES_OF_FILE_SIZE   4
#define BYTES_OF_FILE_TIME   sizeof(FILETIME)

struct ArenaBlockHeader {
    size_t used;
    size_t capacity;
    ArenaBlockHeader* next;
};

struct ArenaAllocator {
    ArenaBlockHeader* head;
    size_t blockSize;
    size_t blockNum;
};

struct FileAttr {
    char* fileName;
    UINT32 fileSize;
    FILETIME lastWriteTime;
    FileAttr* next;
};

struct FileAttrList {
    FileAttr* head;
    FileAttr* tail;
    size_t length;
};

struct PakHeader {
    UCHAR magic[BYTES_OF_MAGIC];
    UCHAR version[BYTES_OF_VERSION];
    FileAttrList flist;
};

struct Resource {
    ArenaAllocator* arena;
    FILE* pakFile;
    FILE* filenameListSav;
};

/*
    create a arena allocator handle.
    remember to call arena_free() at last.
*/
ArenaAllocator* arena_create(size_t blockSize) {
    ArenaAllocator* arena = (ArenaAllocator*)malloc(sizeof(ArenaAllocator));

    if (arena == NULL) {
        return NULL;
    }

    arena->head = (ArenaBlockHeader*)malloc(sizeof(ArenaBlockHeader) + blockSize);
    if (arena->head == NULL) {
        free(arena);
        return NULL;
    }

    arena->blockSize = blockSize;
    arena->head->capacity = blockSize;
    arena->head->used = 0;
    arena->head->next = NULL;
    arena->blockNum = 1;

    return arena;
}

/*
    free all blocks and arena itself.
    this function will do nothing if arena is NULL.
*/
void arena_free(ArenaAllocator* arena) {
    ArenaBlockHeader* cursor;

    if (arena != NULL) {
        cursor = arena->head;

        while (cursor != NULL) {
            arena->head = cursor->next;
            free(cursor);
            cursor = arena->head;
        }

        free(arena);
    }
}

/*
    create a new block with given params.
*/
ArenaBlockHeader* arena_create_new_block(ArenaAllocator* arena, size_t size, size_t used) {
    ArenaBlockHeader* newBlock = (ArenaBlockHeader*)malloc(sizeof(ArenaBlockHeader) + size);
    
    if (newBlock != NULL) {
        newBlock->capacity = size;
        newBlock->used = used;
        newBlock->next = arena->head;
        arena->head = newBlock;

        arena->blockNum += 1;
    }
    else {
        fprintf(stderr, "[ERROR] arena allocator: out of memory\n");
        fflush(stderr);
        abort();
    }

    return newBlock;
}

/*
    same usage as malloc().
*/
void* arena_malloc(ArenaAllocator* arena, size_t size) {
    ArenaBlockHeader* cursor = arena->head;
    ArenaBlockHeader* newBlock;

    while (cursor != NULL) {
        if (cursor->used + size <= cursor->capacity) {
            cursor->used += size;
            return (void*)((char*)(cursor + 1) + cursor->used - size);
        }

        cursor = cursor->next;
    }

    /* if can't find, create a new block. */
    if (size < arena->blockSize) {
        newBlock = arena_create_new_block(arena, arena->blockSize, size);
    }
    else {
        newBlock = arena_create_new_block(arena, size, size);
    }

    return (void*)(newBlock + 1);
}

/*
    clean the resources used in this extractor.
*/
void resource_free(Resource* res) {
    if (res->pakFile != NULL) {
        fclose(res->pakFile);
    }

    if (res->filenameListSav != NULL) {
        fclose(res->filenameListSav);
    }

    arena_free(res->arena);
}

BOOL resource_init(Resource* res, const char* pakFilePath, const char* filenameListSavPath) {
    res->arena = arena_create(8192);
    if (res->arena == NULL) {
        return FALSE;
    }

    res->pakFile = fopen(pakFilePath, "rb");
    if (res->pakFile == NULL) {
        fprintf(stderr, "[ERROR] `%s` is not a valid pak file\n", pakFilePath);
        goto clean_arena;
    }

    res->filenameListSav = fopen(filenameListSavPath, "w");
    if (res->filenameListSav == NULL) {
        fprintf(stderr, "[ERROR] `%s` is not a valid save path\n", filenameListSavPath);
        goto clean_pak_file;
    }

    return TRUE;

clean_pak_file:
    fclose(res->pakFile);
clean_arena:
    arena_free(res->arena);

    return FALSE;
}

void file_attr_list_init(FileAttrList* flist) {
    flist->head = flist->tail = NULL;
    flist->length = 0;
}

void file_attr_list_add(FileAttrList* flist, FileAttr* attr) {
    attr->next = NULL;
    flist->length += 1;

    if (flist->head == NULL) {
        flist->head = flist->tail = attr;
    }
    else {
        flist->tail->next = attr;
        flist->tail = attr;
    }
}

void pak_header_init(PakHeader* header) {
    file_attr_list_init(&(header->flist));
}

/***************** parse. ****************/
#define decode_one_byte(c) \
    (unsigned char)(c ^ 0xf7)

#define decode_bytes(fromBuf, toBuf, len) do { \
    UINT32 i = 0; \
 \
    for (i = 0; i < (len); ++i) { \
        (toBuf)[i] = decode_one_byte((fromBuf)[i]); \
    } \
} while(0)

/* must be 0xc0, 0x4a, 0xc0, 0xba. */
void parse_magic(Resource* res, PakHeader* header) {
    fread(header->magic, sizeof(UCHAR), BYTES_OF_MAGIC, res->pakFile);
    decode_bytes(header->magic, header->magic, BYTES_OF_MAGIC);
}

/* must be 0x00, 0x00, 0x00, 0x00. */
void parse_version(Resource* res, PakHeader* header) {
    fread(header->version, sizeof(UCHAR), BYTES_OF_VERSION, res->pakFile);
    decode_bytes(header->version, header->version, BYTES_OF_VERSION);
}

BOOL reach_pak_header_end(Resource* res) {
    UCHAR flag;
    fread(&flag, sizeof(UCHAR), 1, res->pakFile);
    return decode_one_byte(flag) == 0x80;
}

void parse_file_name(Resource* res, FileAttr* attr) {
    UCHAR byte;
    UINT32 filenameLen;

    /* get the length of the file name. */
    fread(&byte, sizeof(UCHAR), 1, res->pakFile);
    filenameLen = (UINT32)decode_one_byte(byte);

    /* get the file name. */
    attr->fileName = (char*)arena_malloc(res->arena, (filenameLen + 1) * sizeof(char));
    attr->fileName[filenameLen] = '\0';
    fread(attr->fileName, sizeof(char), filenameLen, res->pakFile);
    decode_bytes(attr->fileName, attr->fileName, filenameLen);
}

void parse_file_size(Resource* res, FileAttr* attr) {
    UCHAR* buf = (UCHAR*)(&(attr->fileSize));
    
    fread(buf, sizeof(UCHAR), BYTES_OF_FILE_SIZE, res->pakFile);
    decode_bytes(buf, buf, BYTES_OF_FILE_SIZE);
}

void parse_file_last_write_time(Resource* res, FileAttr* attr) {
    UCHAR* buf = (UCHAR*)(&(attr->lastWriteTime));

    fread(buf, sizeof(UCHAR), BYTES_OF_FILE_TIME, res->pakFile);
    decode_bytes(buf, buf, BYTES_OF_FILE_TIME);
}

void parse_all_file_attrs(Resource* res, PakHeader* header) {
    FileAttr* attr;

    while (!feof(res->pakFile)) {
        if (reach_pak_header_end(res)) {
            break;
        }

        attr = (FileAttr*)arena_malloc(res->arena, sizeof(FileAttr));
        parse_file_name(res, attr);
        parse_file_size(res, attr);
        parse_file_last_write_time(res, attr);

        file_attr_list_add(&(header->flist), attr);
    }
}

void parse_pak_header(Resource* res, PakHeader* header) {
    parse_magic(res, header);
    parse_version(res, header);
    parse_all_file_attrs(res, header);
}

/***************** saving. ****************/
void build_complete_path(char* buf, size_t len, const char* extractPath, const char* fileName) {
    while (*extractPath != '\0') {
        *buf = *extractPath;

        ++buf;
        ++extractPath;
    }

    --extractPath;
    if (*extractPath != '\\') {
        *buf = '\\';
        ++buf;
    }

    while (*fileName != '\0') {
        *buf = *fileName;

        ++buf;
        ++fileName;
    }

    *buf = '\0';
}

BOOL is_dir_exist(const char* path) {
  DWORD dwAttrib = GetFileAttributes(path);

  return (dwAttrib != INVALID_FILE_ATTRIBUTES && 
         (dwAttrib & FILE_ATTRIBUTE_DIRECTORY));
}

/*
    create all parent directories from the given path.
*/
BOOL recursive_create_parent_dirs(char* path) {
    char* cursor = path;

    while (*cursor != '\0') {
        if (*cursor == '\\') {
            /* split a substr here, just make it ends with '\0'. */
            *cursor = '\0';

            if (!is_dir_exist(path)) {
                if (!CreateDirectory(path, NULL)) {
                    return FALSE;
                }
            }

            /* setting back. */
            *cursor = '\\';
        }

        ++cursor;
    }

    return TRUE;
}

void parse_and_extract_one_file(Resource* res, FileAttr* attr, const char* extractPath, char* buf, size_t len) {
    char path[MAX_PATH];
    UINT32 fileSize = attr->fileSize;
    UINT32 readLen;
    HANDLE hFile;
    
    build_complete_path(path, MAX_PATH, extractPath, attr->fileName);
    
    if (!recursive_create_parent_dirs(path)) {
        fprintf(stderr, "[ERROR] can't create parent dirs for `%s`\n", path);
        return;
    }

    hFile = CreateFile(path, 
                    GENERIC_WRITE,
                    0,
                    NULL,
                    CREATE_NEW,
                    FILE_ATTRIBUTE_NORMAL,
                    NULL);
        
    if (hFile == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "[ERROR] CreateFile() failed on `%s`\n", path);
        return;
    }

    while (fileSize > 0) {
        if (fileSize < len) {
            readLen = fread(buf, sizeof(char), fileSize, res->pakFile);
        }
        else {
            readLen = fread(buf, sizeof(char), len, res->pakFile);
        }

        decode_bytes(buf, buf, readLen);
        
        if (!WriteFile(hFile, buf, readLen, NULL, NULL)) {
            fprintf(stderr, "[ERROR] WriteFile() failed\n");
            goto tidy_up;
        }

        fileSize -= readLen;
    }

    if (!SetFileTime(hFile, NULL, NULL, &(attr->lastWriteTime))) {
        fprintf(stderr, "[ERROR] SetFileTime() failed\n");
        goto tidy_up;
    }

tidy_up:
    CloseHandle(hFile);
}

void save_file_name_list(Resource* res, PakHeader* header, const char* savPath) {
    FileAttr* attr = header->flist.head;

    while (attr != NULL) {
        fprintf(res->filenameListSav, "%s, %ld\n", attr->fileName, attr->fileSize);
        attr = attr->next;
    }

    printf("[SUCCESS] file name list is saved at `%s`.\n", savPath);
}

void extract_files(Resource* res, PakHeader* header, const char* extractPath) {
    FileAttr* attr = header->flist.head;
    size_t buf_size = 8192;
    char* buf = (char*)arena_malloc(res->arena, buf_size * sizeof(char));

    while (attr != NULL) {
        parse_and_extract_one_file(res, attr, extractPath, buf, buf_size);
        attr = attr->next;
    }

    printf("[SUCCESS] files are saved at `%s`.\n", extractPath);
}

int main(int argc, char* argv[]) {
    Resource res;
    PakHeader header;

    if (argc != 3) {
        fprintf(stderr, "if you have a .pak file called `main.pak`, and you want to extract it to\n");
        fprintf(stderr, " a dir called `extract_dir`, then usage is: %s main.pak extract_dir\n", argv[0]);
        return 1;
    }

    if (is_dir_exist(argv[2])) {
        fprintf(stderr, "given dir is exists: %s\n", argv[2]);
        return 1;
    }

    if (!resource_init(&res, argv[1], "filenames.txt")) {
        fprintf(stderr, "[ERROR] can't init resources\n");
        return 1;
    }

    pak_header_init(&header);
    parse_pak_header(&res, &header);

    printf("[SUCCESS] `%s` has %d files\n", argv[1], header.flist.length);
    save_file_name_list(&res, &header, "filenames.txt");

    printf("saving files ...\n");
    extract_files(&res, &header, argv[2]);

    resource_free(&res);
    return 0;
}
