/*
  Native File Dialog

  User API

  http://www.frogtoss.com/labs
 */


#ifndef _NFD_H
#define _NFD_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>

/* denotes UTF-8 char */
typedef char nfdchar_t;

/* opaque data structure -- see NFD_PathSet_* */
typedef struct {
    nfdchar_t *buf;
    size_t *indices; /* byte offsets into buf */
    size_t count;    /* number of indices into buf */
}nfdpathset_t;

typedef enum {
    NFD_ERROR,       /* programmatic error */
    NFD_OKAY,        /* user pressed okay, or successful return */
    NFD_CANCEL       /* user pressed cancel */
}nfdresult_t;
    

/* LOCAL ADDITION (not upstream nfd).

   Set the window the dialogs should belong to, as the platform's native handle
   (an HWND on Windows), or NULL to go back to an ownerless dialog.

   Upstream always calls IFileDialog::Show(NULL). With no owner, Windows neither
   disables the calling window nor keeps the dialog in front of it: the dialog
   could end up *behind* the app, which reads as a freeze because the call that
   raised it is blocking, and the app window went on taking clicks -- so a
   second press of the same button opened a second dialog on top of the first.

   The handle is stored, not retained; the caller must clear it (or set a new
   one) before the window it names is destroyed. Backends with no notion of an
   owner window implement this as a no-op -- nfd_zenity.c does, since zenity is
   a separate process. Implemented in nfd_win.cpp and nfd_zenity.c, the two
   backends CMakeLists.txt builds; nfd_cocoa.m and nfd_gtk.c are vendored but
   never compiled, and would each need a definition before they could be. */
void NFD_SetParentWindow( void *nativeWindowHandle );

/* nfd_<targetplatform>.c */

/* single file open dialog */
nfdresult_t NFD_OpenDialog( const nfdchar_t *filterList,
                            const nfdchar_t *defaultPath,
                            nfdchar_t **outPath );

/* multiple file open dialog */    
nfdresult_t NFD_OpenDialogMultiple( const nfdchar_t *filterList,
                                    const nfdchar_t *defaultPath,
                                    nfdpathset_t *outPaths );

/* save dialog */
nfdresult_t NFD_SaveDialog( const nfdchar_t *filterList,
                            const nfdchar_t *defaultPath,
                            nfdchar_t **outPath );


/* select folder dialog */
nfdresult_t NFD_PickFolder( const nfdchar_t *defaultPath,
                            nfdchar_t **outPath);

/* nfd_common.c */

/* get last error -- set when nfdresult_t returns NFD_ERROR */
const char *NFD_GetError( void );
/* get the number of entries stored in pathSet */
size_t      NFD_PathSet_GetCount( const nfdpathset_t *pathSet );
/* Get the UTF-8 path at offset index */
nfdchar_t  *NFD_PathSet_GetPath( const nfdpathset_t *pathSet, size_t index );
/* Free the pathSet */    
void        NFD_PathSet_Free( nfdpathset_t *pathSet );


#ifdef __cplusplus
}
#endif

#endif
