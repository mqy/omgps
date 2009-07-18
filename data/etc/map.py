##########################################################################################
# How to add a new map?
# 1. give it a name, e.g, "OSM"
# 2. add it to map list, with ';' as separator. see function map_list()
# 3. create two functions, one named <map_name>(), another named <map_name>_url().
#
# Function <map_name>() is used to configure the map, where image-type is used to 
# (1) verify content-type of HTTP download, (2) and as file extension
#
# Function <map_name>_url() is used to format url for downloading. 
#
# Please NOTE: 
# 1. respect to the map licenses!
# 2. Modification will not take effect during omgps running.
#    Test your modification before start/restart omgps.
##########################################################################################

def map_list():
	return "OSM; OpenCycle; GoogleMap; YahooSat"

##########################################################################################
def OSM():
	return "min-zoom=1; max-zoom=17; image-type=png"

def OSM_url(zoom, x, y):
	return "http://tile.openstreetmap.org/" + `zoom` + "/" + `x` + "/" + `y` + ".png"

##########################################################################################
def OpenCycle():
	return "min-zoom=0; max-zoom=17; image-type=png"
   
def OpenCycle_url(zoom, x, y):
	return "http://a.andy.sandbox.cloudmade.com/tiles/cycle/" + `zoom` + "/" + `x` + "/" + `y` + ".png"

##########################################################################################
def GoogleMap():
	return "min-zoom=1; max-zoom=17; image-type=png"

def GoogleMap_url(zoom, x, y):
	zoom = 17 - zoom
	return "http://mt.google.com/mt/v=w2.92&hl=en&x=" + `x` + "&y=" + `y` + "&zoom=" + `zoom` + "&s=Gali"

##########################################################################################
def YahooSat():
	return "min-zoom=1; max-zoom=17; image-type=jpg"

def YahooSat_url(zoom, x, y):
	y = (1 << (zoom-1)) - 1 - y
	zoom += 1
	return "http://aerial.maps.yimg.com/ximg?t=a&v=1.9&s=256&x=" + `x` + "&y=" + `y` + "&z=" + `zoom` + "&r=1"
