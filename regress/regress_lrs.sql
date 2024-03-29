-- No M value
select '2d',asewkt(locate_along_measure('POINT(1 2)', 1));
select '3dz',asewkt(locate_along_measure('POINT(1 2 3)', 1));

-- Points
select 'PNTM_1',asewkt(locate_along_measure('POINTM(1 2 3)', 1));
select 'PNTM_2',asewkt(locate_along_measure('POINTM(1 2 3)', 3));
select 'PNTM_3',asewkt(locate_between_measures('POINTM(1 2 3)', 2, 3));
select 'PNTM_4',asewkt(locate_between_measures('POINTM(1 2 3)', 3, 4));
select 'PNTM_5',asewkt(locate_between_measures('POINTM(1 2 4.00001)', 3, 4));

-- Multipoints
select 'MPNT_1',asewkt(locate_between_measures('MULTIPOINTM(1 2 2)', 2, 5));
select 'MPNT_2', asewkt(locate_between_measures('MULTIPOINTM(1 2 8, 2 2 5, 2 1 0)', 2, 5));
select 'MPNT_3', asewkt(locate_between_measures('MULTIPOINTM(1 2 8, 2 2 5.1, 2 1 0)', 2, 5));
select 'MPNT_4', asewkt(locate_between_measures('MULTIPOINTM(1 2 8, 2 2 5, 2 1 0)', 4, 8));

-- Linestrings
select 'LINEZM_1', asewkt(locate_between_measures('LINESTRING(0 10 100 0, 0 0 0 10)', 2, 18));
select 'LINEZM_2', asewkt(locate_between_measures('LINESTRING(0 10 0 0, 0 0 100 10)', 2, 18));
select 'LINEZM_3', asewkt(locate_between_measures('LINESTRING(0 10 100 0, 0 0 0 10, 10 0 0 0)', 2, 18));
select 'LINEZM_4', asewkt(locate_between_measures('LINESTRING(0 10 100 0, 0 0 0 20, 10 0 0 0)', 2, 18));
select 'LINEZM_5', asewkt(locate_between_measures('LINESTRING(0 10 100 0, 0 0 0 20, 0 10 10 40, 10 0 0 0)', 2, 18));
select 'LINEZM_6', asewkt(locate_between_measures('LINESTRING(0 10 10 40, 10 0 0 0)', 2, 2));



-- Linestring, locating point, border cases
select 'LINEZM_7', asewkt(locate_between_measures('LINESTRING(0 0 0 0, 1 1 1 1, 2 2 2 2, 3 4 5 6)', 1, 2));
select 'LINEZM_8', asewkt(locate_between_measures('LINESTRING(0 0 0 0, 1 1 1 1, 2 2 2 2, 3 4 5 6)', 1.1, 2.1));
select 'LINEZM_9', asewkt(locate_between_measures('LINESTRING(0 0 0 0, 1 1 1 1, 2 2 2 2, 3 4 5 6)', 2, 2));


select 'LINEPZM_1', asewkt(locate_along_measure('LINESTRING(0 0 0 0, 1 1 1 1, 2 2 2 2, 3 4 5 6)', 0));
select 'LINEPZM_2', asewkt(locate_along_measure('LINESTRING(0 0 0 0, 1 1 1 1, 2 2 2 2, 3 4 5 6)', 1));
select 'LINEPZM_3', asewkt(locate_along_measure('LINESTRING(0 0 0 0, 1 1 1 1, 2 2 2 2, 3 4 5 6)', 1.5));
select 'LINEPZM_4', asewkt(locate_along_measure('LINESTRING(0 0 0 0, 1 1 1 1, 2 2 2 2, 3 4 5 6)', 2));
select 'LINEPZM_5', asewkt(locate_along_measure('LINESTRING(0 0 0 0, 1 1 1 1, 2 2 2 2, 3 4 5 6)', -1));
select 'LINEPZM_6', asewkt(locate_along_measure('LINESTRING(0 0 0 0, 1 1 1 1, 2 2 2 2, 3 4 5 6)', 6.1));






--- line_locate_point

SELECT 'line_locate_point_1', line_locate_point('LINESTRING(709243.393033887 163969.752725768,708943.240904444 163974.593889146,708675.634380651 163981.832927298)', 'POINT(705780 15883)');

--- postgis-users/2006-January/010613.html
select 'line_locate_point_2', line_locate_point(geomfromtext('LINESTRING(-1953743.873 471070.784,-1953735.105 471075.419,-1953720.034 471081.649)', 6269), geomfromtext('POINT(-1953720.034 471081.649)', 6269));
select 'line_locate_point_3', line_locate_point(geomfromtext('LINESTRING(-1953743.873 471070.784,-1953735.105 471075.419,-1953720.034 471081.649)', 6269), geomfromtext('POINT(-1953743.873 471070.784)', 6269));
--- http://trac.osgeo.org/postgis/ticket/1772#comment:2
select 'line_locate_point_4', line_locate_point('LINESTRING(0 1, 0 1, 0 1)', 'POINT(0 1)');

--- line_substring / line_interpolate_point

--- postgis-devel/2006-January/001951.html
select 'line_substring', asewkt(line_substring(geomfromewkt('SRID=4326;LINESTRING(0 0 0 0, 1 1 1 1, 2 2 2 2, 3 3 3 3, 4 4 4 4)'), 0.5, 0.8));

select 'line_substring', asewkt(line_substring('LINESTRING(0 0 0 0, 1 1 1 1, 2 2 2 2, 3 3 3 3, 4 4 4 4)', 0.5, 0.75));
select 'line_substring', asewkt(line_substring('LINESTRING(0 0, 1 1, 2 2)', 0, 0.5));
select 'line_substring', asewkt(line_substring('LINESTRING(0 0, 1 1, 2 2)', 0.5, 1));
select 'line_substring', asewkt(line_substring('LINESTRING(0 0, 2 2)', 0.5, 1));
select 'line_substring', asewkt(line_substring('LINESTRING(0 0, 2 2)', 0, 0.5));
select 'line_substring', asewkt(line_substring('LINESTRING(0 0, 4 4)', .25, 0.5));
select 'line_substring', asewkt(line_substring('LINESTRINGM(0 0 0, 4 4 4)', .25, 0.5));
select 'line_substring', asewkt(line_substring('LINESTRINGM(0 0 4, 4 4 0)', .25, 0.5));
select 'line_substring', asewkt(line_substring('LINESTRING(0 0 4, 4 4 0)', .25, 0.5));

select 'line_substring', asewkt(line_substring('LINESTRING(0 0, 1 1)', 0, 0));
select 'line_substring', asewkt(line_substring('LINESTRING(0 0 10, 1 1 5)', 0.5, .5));

--- line_interpolate_point

select 'line_interpolate_point', asewkt(line_interpolate_point('LINESTRING(0 0, 1 1)', 0));
select 'line_interpolate_point', asewkt(line_interpolate_point('LINESTRING(0 0 10, 1 1 5)', 0.5));

-- Repeat all tests with the new function names.
-- No M value
select '2d',ST_asewkt(ST_locate_along_measure('POINT(1 2)', 1));
select '3dz',ST_asewkt(ST_locate_along_measure('POINT(1 2 3)', 1));

-- Points
select 'PNTM_1',ST_asewkt(ST_locate_along_measure('POINTM(1 2 3)', 1));
select 'PNTM_2',ST_asewkt(ST_locate_along_measure('POINTM(1 2 3)', 3));
select 'PNTM_3',ST_asewkt(ST_locate_between_measures('POINTM(1 2 3)', 2, 3));
select 'PNTM_4',ST_asewkt(ST_locate_between_measures('POINTM(1 2 3)', 3, 4));
select 'PNTM_5',ST_asewkt(ST_locate_between_measures('POINTM(1 2 4.00001)', 3, 4));

-- Multipoints
select 'MPNT_1',ST_asewkt(ST_locate_between_measures('MULTIPOINTM(1 2 2)', 2, 5));
select 'MPNT_2', ST_asewkt(ST_locate_between_measures('MULTIPOINTM(1 2 8, 2 2 5, 2 1 0)', 2, 5));
select 'MPNT_3', ST_asewkt(ST_locate_between_measures('MULTIPOINTM(1 2 8, 2 2 5.1, 2 1 0)', 2, 5));
select 'MPNT_4', ST_asewkt(ST_locate_between_measures('MULTIPOINTM(1 2 8, 2 2 5, 2 1 0)', 4, 8));

-- Linestrings
select 'LINEZM_1', ST_asewkt(ST_locate_between_measures('LINESTRING(0 10 100 0, 0 0 0 10)', 2, 18));
select 'LINEZM_2', ST_asewkt(ST_locate_between_measures('LINESTRING(0 10 0 0, 0 0 100 10)', 2, 18));
select 'LINEZM_3', ST_asewkt(ST_locate_between_measures('LINESTRING(0 10 100 0, 0 0 0 10, 10 0 0 0)', 2, 18));
select 'LINEZM_4', ST_asewkt(ST_locate_between_measures('LINESTRING(0 10 100 0, 0 0 0 20, 10 0 0 0)', 2, 18));
select 'LINEZM_5', ST_asewkt(ST_locate_between_measures('LINESTRING(0 10 100 0, 0 0 0 20, 0 10 10 40, 10 0 0 0)', 2, 18));
select 'LINEZM_6', ST_asewkt(ST_locate_between_measures('LINESTRING(0 10 10 40, 10 0 0 0)', 2, 2));

--- line_locate_point

SELECT 'st_line_locate_point_1', ST_line_locate_point('LINESTRING(709243.393033887 163969.752725768,708943.240904444 163974.593889146,708675.634380651 163981.832927298)', 'POINT(705780 15883)');

--- postgis-users/2006-January/010613.html
select 'st_line_locate_point_2', ST_line_locate_point(ST_geomfromtext('LINESTRING(-1953743.873 471070.784,-1953735.105 471075.419,-1953720.034 471081.649)', 6269), ST_geomfromtext('POINT(-1953720.034 471081.649)', 6269));
select 'st_line_locate_point_3', ST_line_locate_point(ST_geomfromtext('LINESTRING(-1953743.873 471070.784,-1953735.105 471075.419,-1953720.034 471081.649)', 6269), ST_geomfromtext('POINT(-1953743.873 471070.784)', 6269));
--- http://trac.osgeo.org/postgis/ticket/1772#comment:2
select 'st_line_locate_point_4', line_locate_point('LINESTRING(0 1, 0 1, 0 1)', 'POINT(0 1)');

--- line_substring / line_interpolate_point

--- postgis-devel/2006-January/001951.html
select 'line_substring', ST_asewkt(ST_line_substring(ST_geomfromewkt('SRID=4326;LINESTRING(0 0 0 0, 1 1 1 1, 2 2 2 2, 3 3 3 3, 4 4 4 4)'), 0.5, 0.8));

select 'line_substring', ST_asewkt(ST_line_substring('LINESTRING(0 0 0 0, 1 1 1 1, 2 2 2 2, 3 3 3 3, 4 4 4 4)', 0.5, 0.75));
select 'line_substring', ST_asewkt(ST_line_substring('LINESTRING(0 0, 1 1, 2 2)', 0, 0.5));
select 'line_substring', ST_asewkt(ST_line_substring('LINESTRING(0 0, 1 1, 2 2)', 0.5, 1));
select 'line_substring', ST_asewkt(ST_line_substring('LINESTRING(0 0, 2 2)', 0.5, 1));
select 'line_substring', ST_asewkt(ST_line_substring('LINESTRING(0 0, 2 2)', 0, 0.5));
select 'line_substring', ST_asewkt(ST_line_substring('LINESTRING(0 0, 4 4)', .25, 0.5));
select 'line_substring', ST_asewkt(ST_line_substring('LINESTRINGM(0 0 0, 4 4 4)', .25, 0.5));
select 'line_substring', ST_asewkt(ST_line_substring('LINESTRINGM(0 0 4, 4 4 0)', .25, 0.5));
select 'line_substring', ST_asewkt(ST_line_substring('LINESTRING(0 0 4, 4 4 0)', .25, 0.5));

select 'line_substring', ST_asewkt(ST_line_substring('LINESTRING(0 0, 1 1)', 0, 0));
select 'line_substring', ST_asewkt(ST_line_substring('LINESTRING(0 0 10, 1 1 5)', 0.5, .5));

--- line_interpolate_point

select 'line_interpolate_point', ST_asewkt(ST_line_interpolate_point('LINESTRING(0 0, 1 1)', 0));
select 'line_interpolate_point', ST_asewkt(ST_line_interpolate_point('LINESTRING(0 0 10, 1 1 5)', 0.5));
