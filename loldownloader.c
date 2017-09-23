#include <curl/curl.h>

#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
    #include <Windows.h>
#elif defined(__linux__) || (defined(__APPLE__) && defined(__MACH__))
    #include <sys/ioctl.h>
    #include <sys/stat.h>
    #include <sys/time.h>
    #include <unistd.h>
    #define MAX_PATH 1024
#endif

#define MAX_LINE_LENGTH 256
#define MAX_URL_LENGTH 256
#define MAX_BIN_COUNT 32
#define DEFAULT_PATH "/releases/live"
#define DEFAULT_URL "l3cdn.riotgames.com"
#define DEFAULT_DEST_FOLDER "lol"

// Structure that holds user-selectable (via launch parameters) program options
typedef struct {
    bool useBINFiles;           // Download BIN_0xXXXXXXXX file archives which contain multiple game files instead of downloading files individually
    bool removeExistingFiles;   // Remove existing files and redownload them
    bool keepBINFiles;          // Don't remove BIN files after extracting game files
    char downloadURL[64];       // e.g. l3cdn.riotgames.com
    char downloadPath[64];      // e.g. /releases/live    
    char gameVersion[64];       // e.g. 0.0.0.130
    char destFolder[64];        // e.g. lol
} Options;

// Information about a specific game file
typedef struct {
    char link[MAX_URL_LENGTH];
    char fileName[MAX_PATH];
    unsigned int BIN;
    unsigned int offsetInBIN;
    unsigned int size;
    int unk;
} FileEntry;

// Information about a specific BIN archive file that holds many game files
typedef struct {
    char link[MAX_URL_LENGTH];
    char fileName[MAX_PATH];
} FileArchiveEntry;

typedef struct ListNode_t {
    union {
        FileEntry* fileEntry;
        FileArchiveEntry* fileArchiveEntry;
    };
    struct ListNode_t* next;
} ListNode;

typedef struct {
    ListNode* head;
    ListNode* tail;
} FileList;

// Some stats
typedef struct {
    int numFilesInPackageManifest;  // Number of game files counted in packagemanifest
    int numBINArchives;             // Number of archive files counted in packagemanifest
    int numBytesFromFileList;       // Sum of game files' sizes
    int numBytesFromBINArchives;    // Sum of file archives' sizes
} Statistics;

// Progress data used in function progress_callback
typedef struct {
    unsigned int timeOld;                   // Last time speed was measured
    unsigned int bytesOld;                  // Bytes that had been downloaded when speed was last measured
    unsigned int avgSpeedInBytesPerSecond;  // "Average" speed used to calculate ETA
    unsigned int bytesAlreadyDownloaded;    // Bytes that had already been downloaded when the download was resumed
} ProgressData;

static CURL *g_CURL; // Global CURL handle used when calling libcurl functions
// Default options
static Options g_options = {.useBINFiles            = true,
                            .removeExistingFiles    = false,
                            .keepBINFiles           = false,
                            .downloadURL            = DEFAULT_URL,
                            .downloadPath           = DEFAULT_PATH,
                            .destFolder             = DEFAULT_DEST_FOLDER};
static FileList g_fileList;
static FileList g_fileArchiveList;
static Statistics g_stats;
static ProgressData g_progressData;

// Externally defined inflate (decompress) function
int inf(FILE *source, FILE *dest);

void list_add(FileList* l, void* data)
{
    ListNode* node = malloc(sizeof(ListNode));
    assert(node);
    node->next = 0;
    node->fileEntry = (FileEntry*)data;
    
    if (!l->head) {
        l->head = node;
    }
    
    if (l->tail) {
        l->tail->next = node;
    }
    l->tail = node;
}

void clear_current_line(int columns)
{
    // TODO: This is probably awful, consider changing it
    printf("\r");
    for (int i = 0; i < columns; i++) {
        printf(" ");
    }
}

void build_progress_string(char* buffer, unsigned int bytesTotal, unsigned int bytesNow)
{
    if (bytesTotal < 1024) {
        // Print B
        sprintf(buffer, "(%u/%u B)", bytesNow, bytesTotal);
    } else if (bytesTotal < 1024 * 1024) {
        // Print KiB
        sprintf(buffer, "(%.2f/%.2f KiB)", bytesNow / 1024.0, bytesTotal / 1024.0);
    } else if (bytesTotal < 1024 * 1024 * 1024) {
        // Print MiB
        sprintf(buffer, "(%.2f/%.2f MiB)", bytesNow / 1024.0 / 1024.0, bytesTotal / 1024.0 / 1024.0);
    } else {
        // Print GiB. Game files or archives probably never reach this size though
        sprintf(buffer, "(%.2f/%.2f GiB)", bytesNow / 1024.0 / 1024.0 / 1024.0, bytesTotal / 1024.0 / 1024.0 / 1024.0);
    }
}

void build_speed_string(char* buffer, unsigned int speedInBytesPerSecond)
{
    unsigned int speed = speedInBytesPerSecond;
    if (speed < 1024) {
        // Print B/s
        sprintf(buffer, "%u B/s", speed);
    } else if (speed < 1024 * 1024) {
        // Print KiB/s
        sprintf(buffer, "%u KiB/s", (unsigned int)(speed / 1024.0));
    } else if (speed < 1024 * 1024 * 1024) {
        // Print MiB/s
        sprintf(buffer, "%.1f MiB/s", speed / 1024.0 / 1024.0);
    } else {
        // Print GiB/s. Do you even need a progress and speed indicator if your connection is this fast?
        sprintf(buffer, "%.2f GiB/s", speed / 1024.0 / 1024.0 / 1024.0);
    }
}

void build_ETA_string(char* buffer, unsigned int bytesTotal, unsigned int bytesNow, unsigned int speedInBytesPerSecond)
{
    unsigned int remaining = bytesTotal - bytesNow;
    int secondsLeft = (int)((float)remaining / speedInBytesPerSecond);
    
    sprintf(buffer, "%02d:%02d:%02d", secondsLeft / (60 * 60), (secondsLeft % (60 * 60)) / 60, (secondsLeft % (60 * 60)) % 60);
}

void build_progress_bar_string(char* buffer, float percentage, unsigned int maxWidth)
{
    static char bar[] = "===================================>";
    if (maxWidth > sizeof(bar)) {
        maxWidth = sizeof(bar);
    }
    int index = (int)(maxWidth * percentage);
    sprintf(buffer, "[%s", &bar[sizeof(bar) - index]);
    for (int i = 0; i < (maxWidth - index); i++) {
        strcat(buffer, " ");
    }
    strcat(buffer, "]");
}

unsigned int get_time_ms()
{
    #ifdef _WIN32
        return GetTickCount();
    #elif defined(__linux__) || (defined(__APPLE__) && defined(__MACH__))
        struct timeval tv;
        if (gettimeofday(&tv, 0) != 0) {
            return 0;
        }
        return (tv.tv_sec * 1000) + (tv.tv_usec / 1000);
    #endif
}

unsigned int get_console_columns()
{
    #ifdef _WIN32
        CONSOLE_SCREEN_BUFFER_INFO csbi;
        GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi);
        return csbi.srWindow.Right - csbi.srWindow.Left + 1;
    #elif defined(__linux__) || (defined(__APPLE__) && defined(__MACH__))
        struct winsize w;
        ioctl(STDOUT_FILENO, TIOCGWINSZ, &w);
        return w.ws_col;
    #endif
}

int progress_callback(void *clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow)
{
    ProgressData* progressData = (ProgressData*)clientp;
    unsigned int bytesNow = (unsigned int)(float)dlnow + progressData->bytesAlreadyDownloaded;
    unsigned int bytesTotal = (unsigned int)(float)dltotal + progressData->bytesAlreadyDownloaded;

    // Calculate download speed every second
    #define SMOOTHING_FACTOR 0.1
    unsigned int timeNow = get_time_ms();
    unsigned int speedInBytesPerSecond;
    if (progressData->timeOld + 1000 <= timeNow) {
        progressData->timeOld = timeNow;
        speedInBytesPerSecond = (unsigned int)(bytesNow - progressData->bytesOld);
        if (progressData->avgSpeedInBytesPerSecond == 0) {
            progressData->avgSpeedInBytesPerSecond = speedInBytesPerSecond;
        }
        progressData->avgSpeedInBytesPerSecond = SMOOTHING_FACTOR * speedInBytesPerSecond + (1 - SMOOTHING_FACTOR) * progressData->avgSpeedInBytesPerSecond;
        progressData->bytesOld = bytesNow;
    } else {
        if (bytesNow < bytesTotal) {
            return 0;
        }
        speedInBytesPerSecond = progressData->avgSpeedInBytesPerSecond;
    }
    
    // Show progress bar and info
    char buffer[64];
    static int lastColumns;
    clear_current_line(lastColumns);
    lastColumns = 0;
    if (bytesTotal < 1.0) {
        bytesTotal = 1;
    }
    float p = ((float)bytesNow / (float)bytesTotal);
    build_progress_bar_string(buffer, p, get_console_columns() / 4);
    lastColumns += printf("\r%3d%% %s", (int)(p * 100), buffer);
    
    build_progress_string(buffer, bytesTotal, bytesNow);
    lastColumns += printf(" %s", buffer);
    
    build_speed_string(buffer, speedInBytesPerSecond);
    lastColumns += printf(" | Speed: %s", buffer);
    
    build_ETA_string(buffer, bytesTotal, bytesNow, progressData->avgSpeedInBytesPerSecond);
    lastColumns += printf(" | ETA: %s", buffer);
    
    fflush(stdout);

    return 0;
}

size_t discard_write_callback(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    return size * nmemb;
}

size_t write_callback(char *ptr, size_t size, size_t nmemb, FILE* destFile)
{
    return fwrite(ptr, size, nmemb, destFile);
}

unsigned int file_size(FILE *file)
{
    // TODO: Maybe change this to a method that is guaranteed to work?
    unsigned int initialPos = ftell(file);
    fseek(file, 0, SEEK_END);
    unsigned int size = ftell(file);
    fseek(file, initialPos, SEEK_SET);
    return size;
}

unsigned int file_size_remote(char* URL)
{
    curl_easy_setopt(g_CURL, CURLOPT_URL, URL);
    curl_easy_setopt(g_CURL, CURLOPT_NOPROGRESS, 1L);
    curl_easy_setopt(g_CURL, CURLOPT_NOBODY, 1L);
    curl_easy_setopt(g_CURL, CURLOPT_HEADERFUNCTION, discard_write_callback);
    curl_easy_setopt(g_CURL, CURLOPT_HEADER, 0L);
    g_progressData = (ProgressData){0};
    curl_easy_perform(g_CURL);
    double filesize;
    curl_easy_getinfo(g_CURL, CURLINFO_CONTENT_LENGTH_DOWNLOAD, &filesize);
    
    // Restore defaults
    curl_easy_setopt(g_CURL, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(g_CURL, CURLOPT_NOBODY, 0L);
    curl_easy_setopt(g_CURL, CURLOPT_HEADERFUNCTION, (void*)0);
    return (unsigned int)filesize;
}

bool file_exists(char* fileName)
{
    FILE* file = fopen(fileName, "rb");
    bool ret = file != 0;
    if (file) {
        fclose(file);
    }
    return ret;
}

void make_directory(char* dirName)
{
    #ifdef _WIN32
        CreateDirectory(dirName, 0);
    #elif defined(__linux__) || (defined(__APPLE__) && defined(__MACH__))
        mkdir(dirName, 0777);
    #endif
}

void make_path(char* path)
{
    //printf("Creating path: %s\n", path);
    char temp[MAX_PATH];
    strcpy(temp, path);
    char* slash = strchr(temp, '/');
    while (slash) {
        *slash = '\0';
        make_directory(temp);
        char* prevSlash = slash;
        slash = strchr(slash + 1, '/');
        *prevSlash = '/';
    }
    make_directory(temp);
}

void extract_from_BIN(FileEntry *entry)
{
    char BINFileName[MAX_PATH];
    sprintf(BINFileName, "%s/BIN_0x%08x", g_options.destFolder, entry->BIN);
    FILE* BINFile = fopen(BINFileName, "rb");
    if (!BINFile) {
        printf("[ERROR]: BIN file not found: %s\n", BINFileName);
        exit(0);
        return;
    }
    
    char dir[MAX_PATH];
    strcpy(dir, entry->fileName);
    char* lastSlash = strrchr(dir, '/');
    if (lastSlash) {
        *lastSlash = '\0';
        make_path(dir);
    }
    
    char finalFileName[MAX_PATH]; // Final file name after decompressing
    strcpy(finalFileName, entry->fileName);
    char* lastDot = strrchr(finalFileName, '.');
    if (lastDot) {
        *lastDot = '\0';
    }
    
    FILE* compressedFile = fopen(entry->fileName, "wb");
    FILE* decompressedFile = fopen(finalFileName, "wb");
    fseek(BINFile, entry->offsetInBIN, SEEK_SET);
    char* temp = malloc(entry->size);
    if (!temp) {
        printf("[ERROR]: Couldn't allocate memory to extract file: %s\n", entry->fileName);
        fclose(BINFile);
        fclose(compressedFile);
        fclose(decompressedFile);
        return;
    }
    if (!fread(temp, 1, entry->size, BINFile)) {
        printf("[ERROR]: Couldn't read from BIN file: %s\n", BINFileName);
        fclose(BINFile);
        fclose(compressedFile);
        fclose(decompressedFile);
        return;
    }
    if (!fwrite(temp, 1, entry->size, compressedFile)) {
        printf("[ERROR]: Couldn't write to file: %s\n", entry->fileName);
        fclose(BINFile);
        fclose(compressedFile);
        fclose(decompressedFile);
        return;
    }
    free(temp);
    fclose(BINFile);
    fclose(compressedFile);
    
    // Decompression
    compressedFile = fopen(entry->fileName, "rb");
    inf(compressedFile, decompressedFile);    
    
    // Cleanup    
    fclose(compressedFile);
    fclose(decompressedFile);    
    // Remove compressed file
    remove(entry->fileName);    
}

void download_BIN_archive(FileArchiveEntry* entry)
{
    if (file_exists(entry->fileName)) {
        if (g_options.removeExistingFiles) {
            remove(entry->fileName);
        } else {
            FILE* archive = fopen(entry->fileName, "rb");
            unsigned int localSize = file_size(archive);
            fclose(archive);
            unsigned int remoteSize = file_size_remote(entry->link);
            if (localSize < remoteSize) {
                printf("[INFO]: Resuming download of %s\n", entry->fileName);
                archive = fopen(entry->fileName, "ab");
                curl_easy_setopt(g_CURL, CURLOPT_RESUME_FROM, localSize);
                curl_easy_setopt(g_CURL, CURLOPT_WRITEDATA, (void*)archive);
                curl_easy_setopt(g_CURL, CURLOPT_URL, entry->link);
                g_progressData = (ProgressData){0};
                g_progressData.bytesAlreadyDownloaded = localSize;
                curl_easy_perform(g_CURL);
                fclose(archive);
                // Restore default
                curl_easy_setopt(g_CURL, CURLOPT_RESUME_FROM, 0L);
            } else if (localSize == remoteSize) {
                printf("[INFO]: %s already exists, skipping download\n", entry->fileName);
            } else {
                printf("[WARNING]: Local %s is bigger than remote file\n", entry->fileName);
            }
            return;
        }
    } else  {
        char dir[MAX_PATH];        
        strcpy(dir, entry->fileName);
        char* lastSlash = strrchr(dir, '/');
        if (lastSlash) {
            *lastSlash = '\0';
            make_path(dir);
        }
        
        FILE* BINFile = fopen(entry->fileName, "wb");
        curl_easy_setopt(g_CURL, CURLOPT_WRITEDATA, (void*)BINFile);
        curl_easy_setopt(g_CURL, CURLOPT_URL, entry->link);
        g_progressData = (ProgressData){0};
        curl_easy_perform(g_CURL);
        fclose(BINFile);
    }
}

void download_individual_file(FileEntry* entry)
{
    char finalFileName[MAX_PATH]; // Final file name after decompressing
    strcpy(finalFileName, entry->fileName);
    char* lastDot = strrchr(finalFileName, '.');
    if (lastDot) {
        *lastDot = '\0';
    }
    
    if (file_exists(finalFileName)) {
        if (g_options.removeExistingFiles) {
            remove(finalFileName);
        } else {
            return;
        }
    }
    
    FILE* compressedFile;
    if (!file_exists(entry->fileName)) {
        char dir[MAX_PATH];
        strcpy(dir, entry->fileName);
        char* lastSlash = strrchr(dir, '/');
        if (lastSlash) {
            *lastSlash = '\0';
            make_path(dir);
        }
        
        compressedFile = fopen(entry->fileName, "wb");
        curl_easy_setopt(g_CURL, CURLOPT_WRITEDATA, (void*)compressedFile);
        curl_easy_setopt(g_CURL, CURLOPT_URL, entry->link);
        g_progressData = (ProgressData){0};
        curl_easy_perform(g_CURL);
        fclose(compressedFile);
    }
    
    compressedFile = fopen(entry->fileName, "rb");
    //printf("\nCreating final file: %s\n", finalFileName);
    FILE* finalFile = fopen(finalFileName, "wb");
    inf(compressedFile, finalFile);
    //printf("Removing file: %s\n", entry->fileName);
    fclose(compressedFile);
    fclose(finalFile);
    remove(entry->fileName);
}

void add_file_entry(FileEntry *entry)
{
    list_add(&g_fileList, (void*)entry);
}

void add_file_archive_entry(FileArchiveEntry* entry)
{
    list_add(&g_fileArchiveList, (void*)entry);
}

void get_files_using_packagemanifest(FILE* packagemanifest)
{
    char line[MAX_LINE_LENGTH];    
    assert(fgets(line, MAX_LINE_LENGTH, packagemanifest));
    if (strcmp(line, "PKG1\r\n") != 0) {
        printf("Invalid header: %s\n", line);
        printf("BAD PACKAGEMANIFEST FILE!\n");
        exit(0);
    }
    
    int fileCount = 0;
    int totalSize = 0;
    int maxLineLength = 0;
    bool hasBIN[MAX_BIN_COUNT] = {0};
    
    //printf("\nStart processing packagemanifest\n");
    while (fgets(line, MAX_LINE_LENGTH, packagemanifest)) {
        if (strlen(line) > maxLineLength) {
            maxLineLength = strlen(line);
        }
        
        // Build file entry that will later be used to get the game file
        FileEntry *entry = malloc(sizeof(FileEntry));
        
        char* token = strtok(line, ",");    // Name
        char link[MAX_URL_LENGTH];
        char* temp = strstr(line, "files/");
        temp = strchr(temp, '/');
        char fileName[MAX_PATH];
        sprintf(fileName, "%s%s", g_options.destFolder, temp);
        sprintf(link, "%s%s%s", g_options.downloadURL, g_options.downloadPath, token);
        
        strncpy(entry->link, link, MAX_URL_LENGTH);
        strncpy(entry->fileName, fileName, MAX_PATH);
        
        token = strtok(0, ",");             // BIN_0xXXXXXXXX
        entry->BIN = strtol(token + 6, 0, 16);
        assert(entry->BIN < MAX_BIN_COUNT);
        hasBIN[entry->BIN] = true;
        
        token = strtok(0, ",");             // Offset in BIN_0xXXXXXXXX
        entry->offsetInBIN = atoi(token);
        
        token = strtok(0, ",");             // Size
        entry->size = atoi(token);
        totalSize += atoi(token);
        
        token = strtok(0, ",");             // Unknown
        entry->unk = atoi(token);
        
        add_file_entry(entry);
        
        fileCount++;
    }
    
    g_stats.numFilesInPackageManifest = fileCount;
    g_stats.numBytesFromFileList = totalSize;
    
    char BINLink[MAX_URL_LENGTH] = {0};
    char BINName[MAX_PATH] = {0};
    unsigned int totalBINFilesSize = 0;
    for (int i = 0; i < MAX_BIN_COUNT; i++) {
        if (hasBIN[i]) {
            g_stats.numBINArchives++;
            sprintf(BINName, "BIN_0x%08x", i);
            sprintf(BINLink, "%s%s%s%s%s%s", g_options.downloadURL, g_options.downloadPath, "/projects/lol_game_client/releases/", g_options.gameVersion, "/packages/files/", BINName);
            totalBINFilesSize += file_size_remote(BINLink);
            //printf("BIN:\n  Link: %s\n  Name: %s\n", BINLink, BINName);
            FileArchiveEntry* entry = malloc(sizeof(FileArchiveEntry));
            strncpy(entry->link, BINLink, MAX_URL_LENGTH);
            sprintf(entry->fileName, "%s/%s", g_options.destFolder, BINName);
            add_file_archive_entry(entry);
        }
    }
    
    g_stats.numBytesFromBINArchives = totalBINFilesSize;
    
    if (totalBINFilesSize != totalSize) {
        printf("[WARNING]: Total sizes don't match!");
    }
    
    printf("\nStats:\n");
    printf("  Total size (sum of individual files' sizes): %d B, %.2f KiB, %.2f MiB, %.2f GiB\n", totalSize, totalSize / 1024.0, totalSize / 1024.0 / 1024.0, totalSize / 1024.0 / 1024.0 / 1024.0);
    printf("  Total size (sum of archive files' sizes):    %d B, %.2f KiB, %.2f MiB, %.2f GiB\n", totalBINFilesSize, totalBINFilesSize / 1024.0, totalBINFilesSize / 1024.0 / 1024.0, totalBINFilesSize / 1024.0 / 1024.0 / 1024.0);
    printf("  Max line length: %d\n", maxLineLength);
    printf("  File count: %d\n", g_stats.numFilesInPackageManifest);
    printf("  BIN file count: %d\n", g_stats.numBINArchives);
    printf("\n");
    
    ListNode* c;
    int i;
    if (g_options.useBINFiles) {
        printf("\nDownloading BIN files...\n");
        for (i = 1, c = g_fileArchiveList.head; c; c = c->next, i++) {
            FileArchiveEntry* entry = c->fileArchiveEntry;
            printf("Downloading: %s (%d/%d)\n", entry->fileName, i, g_stats.numBINArchives);
            download_BIN_archive(entry);
            printf("\n");
        }
    }
    
    printf("%s game files...\n", g_options.useBINFiles ? "Extracting" : "Downloading");    
    static int lastColumns = 0;
    char buffer[64];
    float percentage;
    for (i = 1, c = g_fileList.head; c; c = c->next, i++) {
        FileEntry* entry = c->fileEntry;
        clear_current_line(lastColumns);
        percentage = ((float)i / (float)g_stats.numFilesInPackageManifest);    
        build_progress_bar_string(buffer, percentage, get_console_columns() / 4);
        lastColumns = printf("\r%3d%% %s (%d/%d)", (int)(percentage * 100), buffer, i, g_stats.numFilesInPackageManifest);
        fflush(stdout);
        if (g_options.useBINFiles) {
            extract_from_BIN(entry);
        } else {
            curl_easy_setopt(g_CURL, CURLOPT_NOPROGRESS, 1L); // No detailed progress indicator for individual files as it would spam too many messages
            download_individual_file(entry);
        }
    }
    
    // Remove BIN files
    if (g_options.useBINFiles && !g_options.keepBINFiles) {
        for (i = 1, c = g_fileArchiveList.head; c; c = c->next, i++) {
            FileArchiveEntry* entry = c->fileArchiveEntry;
            remove(entry->fileName);
        }
    }
}

char* replace_char(char* string, char c, char replace)
{
    char* stringStart = string;
    while (*string) {
        if (*string == c) {
            *string = replace;
        }
        string++;
    }
    return stringStart;
}

int main(int argc, char *argv[])
{
    CURLcode ret = CURLE_OK;
    
    // Parse program parameters
    bool hasSpecifiedGameVersion = false;
    char* programName = argv[0];
    if (argc > 1) {
        for (int i = 1; i < argc; i++) {
            if (!strcmp("-u", argv[i])) {
                strcpy(g_options.downloadURL, argv[++i]);
            } else if (!strcmp("-p", argv[i])) {
                strcpy(g_options.downloadPath, argv[++i]);
            } else if (!strcmp("-v", argv[i])) {
                strcpy(g_options.gameVersion, argv[++i]);
                hasSpecifiedGameVersion = true;
            } else if (!strcmp("-d", argv[i])) {
                strcpy(g_options.destFolder, replace_char(argv[++i], '\\', '/')); // Replace backslashes in the path with slashes
            } else if (!strcmp("-h", argv[i])) {
                printf("Usage: %s [options] -v VERSION\n", programName);
                printf("  -v VERSION\t: Download game version specified in VERSION\n");
                printf("Options:\n");
                printf("  -u URL\t: Use URL as download URL (default: %s)\n", DEFAULT_URL);
                printf("  -p PATH\t: Use PATH as download path (default: %s)\n", DEFAULT_PATH);
                printf("  -d DIRECTORY\t: Store downloaded files in DIRECTORY (default: %s)\n", DEFAULT_DEST_FOLDER);
                printf("  -h\t\t: Print this help text and exit\n");
                printf("  -i\t\t: (NOT RECOMMENDED) Download files individually instead of extracting them from BIN archives (default: disabled)\n");
                printf("  -r\t\t: Remove existing files and download them again (default: disabled)\n");
                printf("  -k\t\t: Keep BIN archive files after extracting game files from them (default: disabled)\n");
                exit(0);
            } else if (!strcmp("-i", argv[i])) {
                g_options.useBINFiles = false;
            } else if (!strcmp("-r", argv[i])) {
                g_options.removeExistingFiles = true;
            } else if (!strcmp("-k", argv[i])) {
                g_options.keepBINFiles = true;
            } else if (argv[i][0] == '-') {
                printf("Unknown option %s\n", argv[i]);
            }
        }
    }
    
    // Game version is a required option
    if (!hasSpecifiedGameVersion) {
        printf("%s: No game version specified, exiting program.\nIf you need help using this program, run: %s -h\n", programName, programName);
        exit(0);
    }
    
    printf("\nOptions are:\n");
    printf("\tURL: %s\n", g_options.downloadURL);
    printf("\tPath: %s\n", g_options.downloadPath);
    printf("\tVersion: %s\n", g_options.gameVersion);
    printf("\tDestination folder: %s\n", g_options.destFolder);
    printf("\tUse BIN files: %s\n", g_options.useBINFiles ?  "YES" : "NO");
    printf("\tRemove existing files: %s\n", g_options.removeExistingFiles ?  "YES" : "NO");
    printf("\tKeep BIN files: %s\n", g_options.keepBINFiles ?  "YES" : "NO");
    printf("\n");
    
    // Setup CURL
    g_CURL = curl_easy_init();
    curl_easy_setopt(g_CURL, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(g_CURL, CURLOPT_XFERINFOFUNCTION, progress_callback);
    curl_easy_setopt(g_CURL, CURLOPT_XFERINFODATA, (void*)&g_progressData); 
    curl_easy_setopt(g_CURL, CURLOPT_WRITEFUNCTION, write_callback);
    
    // Download packagemanifest
    char packagemanifestURL[MAX_URL_LENGTH];
    char packagemanifestPath[MAX_PATH];
    FILE* packagemanifest;
    strcpy(packagemanifestURL, g_options.downloadURL);
    strcat(packagemanifestURL, g_options.downloadPath);
    strcat(packagemanifestURL, "/projects/lol_game_client/releases/");
    strcat(packagemanifestURL, g_options.gameVersion);
    strcat(packagemanifestURL, "/packages/files/packagemanifest");
    strcpy(packagemanifestPath, g_options.destFolder);
    strcat(packagemanifestPath, "/");
    make_path(packagemanifestPath);
    strcat(packagemanifestPath, "packagemanifest");
    
    if (!file_exists(packagemanifestPath)) {
        printf("[INFO]: packagemanifest not found, downloading it...\n");
        packagemanifest = fopen(packagemanifestPath, "wb");
        curl_easy_setopt(g_CURL, CURLOPT_WRITEDATA, (void*)packagemanifest);
        curl_easy_setopt(g_CURL, CURLOPT_URL, packagemanifestURL);
        g_progressData = (ProgressData){0};
        ret = curl_easy_perform(g_CURL);
        fclose(packagemanifest);
    } else {
        packagemanifest = fopen(packagemanifestPath, "rb");
        unsigned int localSize = file_size(packagemanifest);
        fclose(packagemanifest);
        unsigned int remoteSize = file_size_remote(packagemanifestURL);
        if (localSize < remoteSize) {
            printf("[INFO]: Resuming download of packagemanifest\n");
            packagemanifest = fopen(packagemanifestPath, "ab");
            curl_easy_setopt(g_CURL, CURLOPT_RESUME_FROM, localSize);
            curl_easy_setopt(g_CURL, CURLOPT_WRITEDATA, (void*)packagemanifest);
            curl_easy_setopt(g_CURL, CURLOPT_URL, packagemanifestURL);
            g_progressData = (ProgressData){0};
            g_progressData.bytesAlreadyDownloaded = localSize;
            ret = curl_easy_perform(g_CURL);
            fclose(packagemanifest);
            // Restore default
            curl_easy_setopt(g_CURL, CURLOPT_RESUME_FROM, 0L);
        } else if (localSize == remoteSize) {
            printf("[INFO]: packagemanifest already exists, skipping download\n");
        } else {
            printf("[WARNING]: Local packagemanifest is bigger than remote packagemanifest\n");
        }
    }
    
    // Download game files
    packagemanifest = fopen(packagemanifestPath, "rb");
    get_files_using_packagemanifest(packagemanifest);
    fclose(packagemanifest);

    // Cleanup
    // TODO: Maybe free() malloc()'ed stuff? Or just assume the OS is gonna do it after the program ends
    curl_easy_cleanup(g_CURL);

    return (int)ret;
}
