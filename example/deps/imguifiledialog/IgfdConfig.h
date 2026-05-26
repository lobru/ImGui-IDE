#define USE_STD_FILESYSTEM

#define USE_EXPLORATION_BY_KEYS
#define IGFD_KEY_UP ImGuiKey_UpArrow
#define IGFD_KEY_DOWN ImGuiKey_DownArrow
#define IGFD_KEY_ENTER ImGuiKey_Enter
#define IGFD_KEY_BACKSPACE ImGuiKey_Backspace

#define USE_DIALOG_EXIT_WITH_KEY
#define IGFD_EXIT_KEY ImGuiKey_Escape

#define searchString "Search:"
#define fileNameString "File name:"
#define dirNameString "Directory path:"

#define fileSizeBytes "bytes"
#define fileSizeKiloBytes "KB"
#define fileSizeMegaBytes "MB"
#define fileSizeGigaBytes "GB"

#define OverWriteDialogTitleString "Confirm overwrite..."
#define OverWriteDialogMessageString "The file already exists!\nWould you like to overwrite it?\n\n"
#define OverWriteDialogConfirmButtonString "OK"

#define okCancelButtonAlignement 1.0f
#define invertOkAndCancelButtons true

// Enable the Places (bookmarks) feature and show its pane by default.
// We populate it with the system drives and a persistent Favourites group
// loaded from .claude/favorites.txt.
#define USE_PLACES_FEATURE
#define PLACES_PANE_DEFAULT_SHOWN true
