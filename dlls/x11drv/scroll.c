/*
 * Scroll windows and DCs
 *
 * Copyright 1993  David W. Metcalfe
 * Copyright 1995, 1996 Alex Korobka
 * Copyright 2001 Alexandre Julliard
 */

#include "config.h"

#include "ts_xlib.h"
#include "ts_xutil.h"

#include "winbase.h"
#include "wingdi.h"
#include "winuser.h"

#include "x11drv.h"
#include "win.h"
#include "debugtools.h"

DEFAULT_DEBUG_CHANNEL(x11drv);


/*************************************************************************
 *             fix_caret
 */
static BOOL fix_caret(HWND hWnd, LPRECT lprc, UINT flags)
{
   HWND hCaret = CARET_GetHwnd();

   if( hCaret )
   {
       RECT rc;
       CARET_GetRect( &rc );
       if( hCaret == hWnd ||
          (flags & SW_SCROLLCHILDREN && IsChild(hWnd, hCaret)) )
       {
           POINT pt;
           pt.x = rc.left;
           pt.y = rc.top;
           MapWindowPoints( hCaret, hWnd, (LPPOINT)&rc, 2 );
           if( IntersectRect(lprc, lprc, &rc) )
           {
               HideCaret(0);
               lprc->left = pt.x;
               lprc->top = pt.y;
               return TRUE;
           }
       }
   }
   return FALSE;
}


/*************************************************************************
 *		ScrollDC   (X11DRV.@)
 * 
 * Only the hrgnUpdate is returned in device coordinates.
 * rcUpdate must be returned in logical coordinates to comply with win API.
 * FIXME: the doc explicitly states the opposite, to be checked
 */
BOOL X11DRV_ScrollDC( HDC hdc, INT dx, INT dy, const RECT *rc,
                      const RECT *clipRect, HRGN hrgnUpdate, LPRECT rcUpdate )
{
    RECT rect, rClip, rSrc;

    TRACE( "%04x %d,%d hrgnUpdate=%04x rcUpdate = %p\n", hdc, dx, dy, hrgnUpdate, rcUpdate );
    if (clipRect) TRACE( "cliprc = (%d,%d,%d,%d)\n",
                         clipRect->left, clipRect->top, clipRect->right, clipRect->bottom );
    if (rc) TRACE( "rc = (%d,%d,%d,%d)\n", rc->left, rc->top, rc->right, rc->bottom );

    /* compute device clipping region (in device coordinates) */

    if (rc) rect = *rc;
    else GetClipBox( hdc, &rect );

    if (clipRect)
    {
        rClip = *clipRect;
        IntersectRect( &rClip, &rect, &rClip );
    }
    else rClip = rect;

    rSrc = rClip;
    OffsetRect( &rSrc, -dx, -dy );
    IntersectRect( &rSrc, &rSrc, &rect );

    if (!IsRectEmpty(&rSrc))
    {
        /* copy bits */
        if (!BitBlt( hdc, rSrc.left + dx, rSrc.top + dy,
                     rSrc.right - rSrc.left, rSrc.bottom - rSrc.top,
                     hdc, rSrc.left, rSrc.top, SRCCOPY))
            return FALSE;
    }

    /* compute update areas */

    if (hrgnUpdate || rcUpdate)
    {
        HRGN hrgn = hrgnUpdate, hrgn2;
        POINT pt;

        /* map everything to device coordinates */
        pt.x = rect.left + dx;
        pt.y = rect.top + dy;
        LPtoDP( hdc, &pt, 1 );
        LPtoDP( hdc, (LPPOINT)&rect, 2 );
        LPtoDP( hdc, (LPPOINT)&rClip, 2 );
        dx = pt.x - rect.left;
        dy = pt.y - rect.top;

        hrgn2 = CreateRectRgnIndirect( &rect );
        if (hrgn) SetRectRgn( hrgn, rClip.left, rClip.top, rClip.right, rClip.bottom );
        else hrgn = CreateRectRgn( rClip.left, rClip.top, rClip.right, rClip.bottom );
        CombineRgn( hrgn, hrgn, hrgn2, RGN_AND );
        OffsetRgn( hrgn2, dx, dy );
        CombineRgn( hrgn, hrgn, hrgn2, RGN_DIFF );

        if( rcUpdate )
        {
            GetRgnBox( hrgn, rcUpdate );

            /* Put the rcUpdate in logical coordinate */
            DPtoLP( hdc, (LPPOINT)rcUpdate, 2 );
        }
        if (!hrgnUpdate) DeleteObject( hrgn );
        DeleteObject( hrgn2 );
    }
    return TRUE;
}


/*************************************************************************
 *		ScrollWindowEx   (X11DRV.@)
 */
INT X11DRV_ScrollWindowEx( HWND hwnd, INT dx, INT dy,
                           const RECT *rect, const RECT *clipRect,
                           HRGN hrgnUpdate, LPRECT rcUpdate, UINT flags )
{
    INT  retVal = NULLREGION;
    BOOL bCaret = FALSE, bOwnRgn = TRUE;
    RECT rc, cliprc;
    WND*   wnd = WIN_FindWndPtr( hwnd );

    if( !wnd || !WIN_IsWindowDrawable( wnd, TRUE ))
    {
        retVal = ERROR;
        goto END;
    }

    GetClientRect(hwnd, &rc);
    if (rect) IntersectRect(&rc, &rc, rect);

    if (clipRect) IntersectRect(&cliprc,&rc,clipRect);
    else cliprc = rc;

    if (!IsRectEmpty(&cliprc) && (dx || dy))
    {
        HDC   hDC;
        BOOL  bUpdate = (rcUpdate || hrgnUpdate || flags & (SW_INVALIDATE | SW_ERASE));
        HRGN  hrgnClip = CreateRectRgnIndirect(&cliprc);
        HRGN  hrgnTemp;
        RECT  caretrc;

        TRACE( "%04x, %d,%d hrgnUpdate=%04x rcUpdate = %p rc=(%d,%d-%d,%d) %04x\n",
               hwnd, dx, dy, hrgnUpdate, rcUpdate,
               rc.left, rc.top, rc.right, rc.bottom, flags );
        if (clipRect) TRACE( "cliprc = (%d,%d,%d,%d)\n",
                             clipRect->left, clipRect->top,
                             clipRect->right, clipRect->bottom );

        caretrc = rc;
        bCaret = fix_caret(hwnd, &caretrc, flags);

        if( hrgnUpdate ) bOwnRgn = FALSE;
        else if( bUpdate ) hrgnUpdate = CreateRectRgn( 0, 0, 0, 0 );

        hDC = GetDCEx( hwnd, 0, DCX_CACHE | DCX_USESTYLE );
        if (hDC)
        {
            HRGN hrgn = CreateRectRgn( 0, 0, 0, 0 );
            X11DRV_StartGraphicsExposures( hDC );
            X11DRV_ScrollDC( hDC, dx, dy, &rc, &cliprc, hrgnUpdate, rcUpdate );
            X11DRV_EndGraphicsExposures( hDC, hrgn );
            ReleaseDC( hwnd, hDC );
            if (bUpdate) CombineRgn( hrgnUpdate, hrgnUpdate, hrgn, RGN_OR );
            else RedrawWindow( hwnd, NULL, hrgn, RDW_INVALIDATE | RDW_ERASE );
            DeleteObject( hrgn );
        }

        /* Take into account the fact that some damages may have occured during the scroll */
        hrgnTemp = CreateRectRgn( 0, 0, 0, 0 );
        if (GetUpdateRgn( hwnd, hrgnTemp, FALSE ) != NULLREGION)
        {
            OffsetRgn( hrgnTemp, dx, dy );
            CombineRgn( hrgnTemp, hrgnTemp, hrgnClip, RGN_AND );
            RedrawWindow( hwnd, NULL, hrgnTemp, RDW_INVALIDATE | RDW_ERASE );
        }
        DeleteObject( hrgnTemp );

        if( flags & SW_SCROLLCHILDREN )
        {
            RECT r;
            WND *w;
            for( w =WIN_LockWndPtr(wnd->child); w; WIN_UpdateWndPtr(&w, w->next))
            {
                r = w->rectWindow;
                if( !rect || IntersectRect(&r, &r, &rc) )
                    SetWindowPos(w->hwndSelf, 0, w->rectWindow.left + dx,
                                 w->rectWindow.top  + dy, 0,0, SWP_NOZORDER |
                                 SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOREDRAW |
                                 SWP_DEFERERASE );
            }
        }

        if( flags & (SW_INVALIDATE | SW_ERASE) )
            RedrawWindow( hwnd, NULL, hrgnUpdate, RDW_INVALIDATE | RDW_ERASE |
                          ((flags & SW_ERASE) ? RDW_ERASENOW : 0) |
                          ((flags & SW_SCROLLCHILDREN) ? RDW_ALLCHILDREN : 0 ) );

        if( bCaret )
        {
            SetCaretPos( caretrc.left + dx, caretrc.top + dy );
            ShowCaret(0);
        }

        if( bOwnRgn && hrgnUpdate ) DeleteObject( hrgnUpdate );
        DeleteObject( hrgnClip );
    }
END:
    WIN_ReleaseWndPtr(wnd);
    return retVal;
}
