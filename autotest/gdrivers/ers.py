#!/usr/bin/env python
###############################################################################
# $Id$
#
# Project:  GDAL/OGR Test Suite
# Purpose:  Test ERS format driver.
# Author:   Frank Warmerdam <warmerdam@pobox.com>
# 
###############################################################################
# Copyright (c) 2007, Frank Warmerdam <warmerdam@pobox.com>
# 
# Permission is hereby granted, free of charge, to any person obtaining a
# copy of this software and associated documentation files (the "Software"),
# to deal in the Software without restriction, including without limitation
# the rights to use, copy, modify, merge, publish, distribute, sublicense,
# and/or sell copies of the Software, and to permit persons to whom the
# Software is furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included
# in all copies or substantial portions of the Software.
# 
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
# OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
# THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
# FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
# DEALINGS IN THE SOFTWARE.
###############################################################################

import os
import sys
import gdal

sys.path.append( '../pymod' )

import gdaltest

###############################################################################
# Perform simple read test.

def ers_1():

    tst = gdaltest.GDALTest( 'ERS', 'srtm.ers', 1, 64074 )
    return tst.testOpen()

###############################################################################
# Create simple copy and check.

def ers_2():

    tst = gdaltest.GDALTest( 'ERS', 'float32.bil', 1, 27 )
    return tst.testCreateCopy( new_filename = 'tmp/float32.ers',
                               check_gt = 1, vsimem = 1 )
    
###############################################################################
# Test multi-band file.

def ers_3():

    tst = gdaltest.GDALTest( 'ERS', 'rgbsmall.tif', 2, 21053 )
    return tst.testCreate( new_filename = 'tmp/rgbsmall.ers' )
    
###############################################################################
# Test HeaderOffset case.

def ers_4():

    gt = (143.62125, 0.025000000000000001, 0.0, -39.40625, 0.0, -0.025)
    
    srs = """GEOGCS["GEOCENTRIC DATUM of AUSTRALIA",
    DATUM["GDA94",
        SPHEROID["GRS80",6378137,298.257222101]],
    PRIMEM["Greenwich",0],
    UNIT["degree",0.0174532925199433]]"""
 
    tst = gdaltest.GDALTest( 'ERS', 'ers_dem.ers', 1, 56588 )
    return tst.testOpen( check_prj = srs, check_gt = gt )
    
###############################################################################
# Confirm we can recognised signed 8bit data.

def ers_5():

    ds = gdal.Open( 'data/8s.ers' )
    md = ds.GetRasterBand(1).GetMetadata('IMAGE_STRUCTURE')

    if md['PIXELTYPE'] != 'SIGNEDBYTE':
        gdaltest.post_reason( 'Failed to detect SIGNEDBYTE' )
        return 'fail'

    ds = None

    return 'success'
    
###############################################################################
# Confirm a copy preserves the signed byte info.

def ers_6():

    drv = gdal.GetDriverByName( 'ERS' )
    
    src_ds = gdal.Open( 'data/8s.ers' )

    ds = drv.CreateCopy( 'tmp/8s.ers', src_ds )
    
    md = ds.GetRasterBand(1).GetMetadata('IMAGE_STRUCTURE')

    if md['PIXELTYPE'] != 'SIGNEDBYTE':
        gdaltest.post_reason( 'Failed to detect SIGNEDBYTE' )
        return 'fail'

    ds = None

    drv.Delete( 'tmp/8s.ers' )
    
    return 'success'
    
###############################################################################
# Test opening a file with everything in lower case.

def ers_7():

    ds = gdal.Open( 'data/caseinsensitive.ers' )

    desc = ds.GetRasterBand(1).GetDescription()

    if desc != 'RTP 1st Vertical Derivative':
        print(desc)
        gdaltest.post_reason( 'did not get expected values.' )
        return 'fail'

    return 'success'

###############################################################################
# Test GCP support

def ers_8():

    src_ds = gdal.Open('../gcore/data/gcps.vrt')
    drv = gdal.GetDriverByName( 'ERS' )
    ds = drv.CreateCopy('/vsimem/ers_8.ers', src_ds)
    ds = None

    gdal.Unlink('/vsimem/ers_8.ers.aux.xml')

    ds = gdal.Open('/vsimem/ers_8.ers')
    expected_gcps = src_ds.GetGCPs()
    gcps = ds.GetGCPs()
    gcp_count = ds.GetGCPCount()
    wkt = ds.GetGCPProjection()
    ds = None

    if wkt != """PROJCS["NUTM11",GEOGCS["NAD27",DATUM["North_American_Datum_1927",SPHEROID["Clarke 1866",6378206.4,294.978698213898,AUTHORITY["EPSG","7008"]],TOWGS84[-3,142,183,0,0,0,0],AUTHORITY["EPSG","6267"]],PRIMEM["Greenwich",0,AUTHORITY["EPSG","8901"]],UNIT["degree",0.0174532925199433,AUTHORITY["EPSG","9108"]],AXIS["Lat",NORTH],AXIS["Long",EAST],AUTHORITY["EPSG","4267"]],PROJECTION["Transverse_Mercator"],PARAMETER["latitude_of_origin",0],PARAMETER["central_meridian",-117],PARAMETER["scale_factor",0.9996],PARAMETER["false_easting",500000],PARAMETER["false_northing",0],UNIT["Meter",1]]""":
        gdaltest.post_reason('did not get expected GCP projection')
        print(wkt)
        return 'fail'

    if len(gcps) != len(expected_gcps) or len(gcps) != gcp_count:
        gdaltest.post_reason('did not get expected GCP number')
        return 'fail'

    for i in range(len(gcps)):
        if abs(gcps[i].GCPPixel - expected_gcps[i].GCPPixel) > 1e-6 or \
           abs(gcps[i].GCPLine - expected_gcps[i].GCPLine) > 1e-6 or \
           abs(gcps[i].GCPX - expected_gcps[i].GCPX) > 1e-6 or \
           abs(gcps[i].GCPY - expected_gcps[i].GCPY) > 1e-6:
            gdaltest.post_reason('did not get expected GCP %d' % i)
            print(gcps[i])
            return 'fail'

    drv.Delete('/vsimem/ers_8.ers')

    return 'success'

###############################################################################
# Test NoData support (#4207)

def ers_9():

    drv = gdal.GetDriverByName( 'ERS' )
    ds = drv.Create('/vsimem/ers_9.ers', 1, 1)
    ds.GetRasterBand(1).SetNoDataValue(123)
    ds = None

    f = gdal.VSIFOpenL('/vsimem/ers_9.ers.aux.xml', 'rb')
    if f is not None:
        gdaltest.post_reason('/vsimem/ers_9.ers.aux.xml should not exist')
        gdal.VSIFCloseL(f)
        drv.Delete('/vsimem/ers_9.ers')
        return 'fail'

    ds = gdal.Open('/vsimem/ers_9.ers')
    val = ds.GetRasterBand(1).GetNoDataValue()
    ds = None

    drv.Delete('/vsimem/ers_9.ers')

    if val != 123:
        gdaltest.post_reason('did not get expected nodata value')
        print(val)
        return 'fail'

    return 'success'

###############################################################################
# Cleanup

def ers_cleanup():
    gdaltest.clean_tmp()
    return 'success'

gdaltest_list = [
    ers_1,
    ers_2,
    ers_3,
    ers_4,
    ers_5,
    ers_6,
    ers_7,
    ers_8,
    ers_9,
    ers_cleanup
    ]
  


if __name__ == '__main__':

    gdaltest.setup_run( 'ers' )

    gdaltest.run_tests( gdaltest_list )

    gdaltest.summarize()

