// Some utility functions for coordinate conversion.
// Convert 1024 coords to standard dpy coords
int
cvt1024ToDpy(int coord)
{
    coord -= 512;
    // we have to take underflow (-513 result) into account
    if( (coord < 0) && (coord != -512) )
    {
        coord--;
    }

    return( coord & 01777 );
}

// Convert a 10 bit 1's complement dpy coordinate to a 2's complement 0-1023 value.
int
cvtDpyTo1024(int dpy)
{
    if( dpy & 01000 )
    {
        dpy++;
    }

    return( (dpy + 01000) & 01777);
}
