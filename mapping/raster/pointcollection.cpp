#include "raster/pointcollection.h"

#include <sstream>
#include <iomanip>


Point::Point(double x, double y, uint16_t size_md_string, uint16_t size_md_value) : x(x), y(y), md_string(size_md_string), md_value(size_md_value) {
}

Point::~Point() {
}

#if 0 // Default implementations should be equal
// Copy constructors
Point::Point(const Point &p) : x(p.x), y(p.y), md_string(p.md_string), md_value(p.md_value) {
}
Point &Point::operator=(const Point &p) {
	x = p.x;
	y = p.y;

	md_string = p.md_string;
	md_value = p.md_value;

	return *this;
}

// Move constructors
Point::Point(Point &&p) : x(p.x), y(p.y), md_string( std::move(p.md_string) ), md_value( std::move(p.md_value) ) {
}
Point &Point::operator=(Point &&p) {
	x = p.x;
	y = p.y;

	md_string = std::move(p.md_string);
	md_value = std::move(p.md_value);

	return *this;
}
#endif


/*
Point PointCollection::makePoint(double x, double y) {
	return Point(x, y, local_string_md.size(), local_double_md.size());
}
*/

PointCollection::PointCollection(epsg_t epsg) : epsg(epsg) {

}
PointCollection::~PointCollection() {

}

Point &PointCollection::addPoint(double x, double y) {
	local_md_string.lock();
	local_md_value.lock();
	//collection.emplace_back(x, y, local_md_string.size(), local_md_value.size());
	collection.push_back(Point(x, y, local_md_string.size(), local_md_value.size()));
	return collection[ collection.size() - 1 ];
}


/**
 * Global Metadata
 */
const std::string &PointCollection::getGlobalMDString(const std::string &key) {
	return global_md_string.get(key);
}

double PointCollection::getGlobalMDValue(const std::string &key) {
	return global_md_value.get(key);
}

DirectMetadata<std::string>* PointCollection::getGlobalMDStringIterator() {
	return &global_md_string;
}

DirectMetadata<double>* PointCollection::getGlobalMDValueIterator() {
	return &global_md_value;
}

std::vector<std::string> PointCollection::getGlobalMDValueKeys() {
	std::vector<std::string> keys;
	for (auto keyValue : global_md_value) {
		keys.push_back(keyValue.first);
	}
	return keys;
}

std::vector<std::string> PointCollection::getGlobalMDStringKeys() {
	std::vector<std::string> keys;
	for (auto keyValue : global_md_string) {
		keys.push_back(keyValue.first);
	}
	return keys;
}

void PointCollection::setGlobalMDString(const std::string &key, const std::string &value) {
	global_md_string.set(key, value);
}

void PointCollection::setGlobalMDValue(const std::string &key, double value) {
	global_md_value.set(key, value);
}


/**
 * Local meta-metadata
 */
void PointCollection::addLocalMDString(const std::string &key) {
	local_md_string.addKey(key);
}

void PointCollection::addLocalMDValue(const std::string &key) {
	local_md_value.addKey(key);
}


/**
 * Local Metadata on points
 */
const std::string &PointCollection::getLocalMDString(const Point &point, const std::string &key) {
	return local_md_string.getValue(point.md_string, key);
}

double PointCollection::getLocalMDValue(const Point &point, const std::string &key) {
	return local_md_value.getValue(point.md_value, key);
}

std::vector<std::string> PointCollection::getLocalMDStringKeys() {
	std::vector<std::string> keys;
	for(auto keyValue : local_md_string) {
		keys.push_back(keyValue.first);
	}
	return keys;
}

std::vector<std::string> PointCollection::getLocalMDValueKeys() {
	std::vector<std::string> keys;
	for (auto keyValue : local_md_value) {
		keys.push_back(keyValue.first);
	}
	return keys;
}

void PointCollection::setLocalMDString(Point &point, const std::string &key, const std::string &value) {
	local_md_string.setValue(point.md_string, key, value);
}

void PointCollection::setLocalMDValue(Point &point, const std::string &key, double value) {
	local_md_value.setValue(point.md_value, key, value);
}

/**
 * Export
 */
#if 0
// http://www.gdal.org/ogr_apitut.html
#include "ogrsf_frmts.h"

void gdal_init(); // implemented in raster/import_gdal.cpp

void PointCollection::toOGR(const char *driver = "ESRI Shapefile") {
	gdal_init();

    GDALDriver *poDriver = GetGDALDriverManager()->GetDriverByName(pszDriverName );
    if (poDriver == nullptr)
    	throw ExporterException("OGR driver not available");

    GDALDataset *poDS = poDriver->Create( "point_out.shp", 0, 0, 0, GDT_Unknown, NULL );
    if (poDS == nullptr) {
    	// TODO: free driver?
    	throw ExporterException("Dataset creation failed");
    }

    OGRLayer *poLayer = poDS->CreateLayer( "point_out", NULL, wkbPoint, NULL );
    if (poLayer == nullptr) {
    	// TODO: free driver and dataset?
    	throw ExporterException("Layer Creation failed");
    }

    // No attributes

    // Loop over all points
	for (const Point &p : collection) {
		double x = p.x, y = p.y;

		OGRFeature *poFeature = OGRFeature::CreateFeature(poLayer->GetLayerDefn());
		//poFeature->SetField( "Name", szName );
		OGRPoint pt;
		pt.setX(p.x);
		pt.setY(p.y);
		poFeature->SetGeometry(&pt);

        if (poLayer->CreateFeature( poFeature ) != OGRERR_NONE) {
        	// TODO: free stuf..
        	throw ExporterException("CreateFeature failed");
        }

        OGRFeature::DestroyFeature( poFeature );
	}
	GDALClose(poDS);
}
#endif


std::string PointCollection::toGeoJSON() {
/*
	  { "type": "MultiPoint",
	    "coordinates": [ [100.0, 0.0], [101.0, 1.0] ]
	    }
*/
	std::ostringstream json;
	json << std::fixed; // std::setprecision(4);

	//json << "{ \"type\": \"MultiPoint\", \"coordinates\": [ ";
	json << "{\"type\":\"FeatureCollection\",\"crs\": {\"type\": \"name\", \"properties\":{\"name\": \"EPSG:" << epsg <<"\"}},\"features\":[{\"type\":\"Feature\",\"geometry\":{\"type\": \"MultiPoint\", \"coordinates\": [ ";

	bool first = true;
	for (const Point &p : collection) {
		if (first)
			first = false;
		else
			json << ", ";

		json << "[" << p.x << "," << p.y << "]";
	}
	//json << "] }";
	json << "] }}]}";

	return json.str();
}

std::string PointCollection::toCSV() {
	std::ostringstream csv;
	csv << std::fixed; // std::setprecision(4);


	//header
	csv << "lon" << "," << "lat";
	for(std::string key : getLocalMDStringKeys()){
		csv << "," << key;
	}
	csv << std::endl;


	bool first = true;
	for (const Point &p : collection) {
		csv << p.x << "," << p.y;

		for(std::string key : getLocalMDStringKeys()){
				csv << "," << getLocalMDString(p, key);
		}

		csv << std::endl;
	}

	return csv.str();
}

PointCollectionMetadataCopier::PointCollectionMetadataCopier(PointCollection& pointsOld, PointCollection& pointsNew) : pointsOld(pointsOld), pointsNew(pointsNew) {
}

void PointCollectionMetadataCopier::copyGlobalMetadata() {
	for (auto pair : *(pointsOld.getGlobalMDValueIterator())) {
		pointsNew.setGlobalMDValue(pair.first, pair.second);
	}
	for (auto pair : *(pointsOld.getGlobalMDStringIterator())) {
		pointsNew.setGlobalMDString(pair.first, pair.second);
	}
}

void PointCollectionMetadataCopier::initLocalMetadataFields() {
	localMDStringKeys = pointsOld.getLocalMDStringKeys();
	for (auto key : localMDStringKeys) {
		pointsNew.addLocalMDString(key);
	}

	localMDValueKeys = pointsOld.getLocalMDValueKeys();
	for (auto key : localMDValueKeys) {
		pointsNew.addLocalMDValue(key);
	}
}

void PointCollectionMetadataCopier::copyLocalMetadata(const Point& pointOld, Point& pointNew) {
	for (auto key : localMDStringKeys) {
		pointsNew.setLocalMDString(pointNew, key, pointsOld.getLocalMDString(pointOld, key));
	}
	for (auto key : localMDValueKeys) {
		pointsNew.setLocalMDValue(pointNew, key, pointsOld.getLocalMDValue(pointOld, key));
	}
}
