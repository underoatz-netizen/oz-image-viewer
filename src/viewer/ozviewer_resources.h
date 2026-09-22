// ozviewer_resources.h - shared numeric resource identifiers.
//
// Included by both ozviewer.rc (resource compiler) and the C++ translation
// units that load a resource by ID, so every value has a single definition and
// cannot drift between the resource script and the code.
#ifndef OZVIEWER_RESOURCES_H
#define OZVIEWER_RESOURCES_H

// Application icon (assets/icons/ozviewer.ico), embedded by ozviewer.rc and
// used for the executable/Shell identity and the top-level window icon. Kept
// clear of the WM_COMMAND menu/button ids (101-108, 201-229) so the icon id
// cannot be confused with a command id during review or maintenance.
#define IDI_OZVIEWER_APPICON 130

#endif // OZVIEWER_RESOURCES_H
