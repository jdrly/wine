/*
 * GW2 on Mac: the session notifications IAudioSessionManager2 keeps (jdrly/gw2-on-mac#387)
 *
 * RegisterSessionNotification and UnregisterSessionNotification were stubs that answered
 * E_NOTIMPL. NAudio 2.0.0, which Blish HUD follows the game's volume with, throws on that, the
 * second time from a finalizer, where nothing catches it and the process ends. Here a manager
 * keeps a reference on each registered notification until it is unregistered or the manager's
 * last reference goes. No OnSessionCreated event is sent: nothing tells a manager about a new
 * session. Only audiosessionmanager.c includes this file. docs/gw2/adr/0025 has the rest.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#ifndef __WINE_MMDEVAPI_SESSIONNOTIFY_H
#define __WINE_MMDEVAPI_SESSIONNOTIFY_H

struct session_notification
{
    struct list entry;
    IAudioSessionNotification *notification;
};

/* keeps a reference on <notification> in <list>, under sessions_lock() */
static inline HRESULT session_notification_add(struct list *list, IAudioSessionNotification *notification)
{
    struct session_notification *entry;

    if (!notification)
        return E_POINTER;
    if (!(entry = malloc(sizeof(*entry))))
        return E_OUTOFMEMORY;

    IAudioSessionNotification_AddRef(notification);
    entry->notification = notification;
    sessions_lock();
    list_add_tail(list, &entry->entry);
    sessions_unlock();
    return S_OK;
}

/* drops <notification> from <list> and its reference, or E_INVALIDARG when it isn't there. The
 * release runs outside the lock, since it may call back into the manager. */
static inline HRESULT session_notification_remove(struct list *list, IAudioSessionNotification *notification)
{
    struct session_notification *cur, *found = NULL;

    if (!notification)
        return E_POINTER;

    sessions_lock();
    LIST_FOR_EACH_ENTRY(cur, list, struct session_notification, entry)
    {
        if (cur->notification == notification)
        {
            list_remove(&cur->entry);
            found = cur;
            break;
        }
    }
    sessions_unlock();

    if (!found)
        return E_INVALIDARG;
    IAudioSessionNotification_Release(found->notification);
    free(found);
    return S_OK;
}

/* drops every notification in <list>, when the manager that owns it goes and nobody else can
 * reach the list */
static inline void session_notifications_release(struct list *list)
{
    struct session_notification *cur, *next;

    LIST_FOR_EACH_ENTRY_SAFE(cur, next, list, struct session_notification, entry)
    {
        IAudioSessionNotification_Release(cur->notification);
        free(cur);
    }
}

#endif /* __WINE_MMDEVAPI_SESSIONNOTIFY_H */
