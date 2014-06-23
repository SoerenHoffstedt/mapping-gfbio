
#include "raster/raster.h"
#include "raster/rastersource.h"
#include "raster/profiler.h"
#include "converters/converter.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>


#include <iostream>
#include <fstream>
#include <sstream>
#include <memory>
#include <string>



#include <json/json.h>
#include <sqlite3.h>


RasterSource::RasterSource(const char *_filename, bool writeable)
	: lockedfile(-1), writeable(writeable), filename_json(_filename), lcrs(nullptr), channelcount(0), channels(nullptr), db(nullptr), refcount(0) {
	try {
		init();
	}
	catch (const std::exception &e) {
		cleanup();
		throw;
	}
}

RasterSource::~RasterSource() {
	cleanup();
}


void RasterSource::init() {
	size_t suffixpos = filename_json.rfind(".json");
	if (suffixpos == std::string::npos || suffixpos != filename_json.length()-5) {
		printf("Filename doesn't end with .json (%ld / %ld)\n", suffixpos, filename_json.length());
		throw SourceException("filename must end with .json");
	}
	std::string basename = filename_json.substr(0, suffixpos);
	filename_data = basename + ".dat";
	filename_db = basename + ".db";

	/*
	 * Step #1: open the .json file and parse it
	 */
	std::ifstream file(filename_json.c_str());
	if (!file.is_open()) {
		printf("unable to open file %s\n", filename_json.c_str());
		throw SourceException("unable to open file");
	}

	Json::Reader reader(Json::Features::strictMode());
	Json::Value root;
	if (!reader.parse(file, root)) {
		printf("unable to read json\n%s\n", reader.getFormattedErrorMessages().c_str());
		throw SourceException("unable to parse json");
	}

	file.close();

	// Now reopen the file to acquire a lock
	lockedfile = open(filename_json.c_str(), O_RDONLY | O_CLOEXEC); // | O_NOATIME | O_CLOEXEC);
	if (lockedfile < 0) {
		printf("Unable to open() rastersource at %s\n", filename_json.c_str());
		perror("error: ");
		throw SourceException("open() before flock() failed");
	}
	if (flock(lockedfile, writeable ? LOCK_EX : LOCK_SH) != 0) {
		printf("Unable to flock() rastersource\n");
		throw SourceException("flock() failed");
	}

	Json::Value jrm = root["coords"];
	Json::Value sizes = jrm["size"];
	Json::Value origins = jrm["origin"];
	Json::Value scales = jrm["scale"];
	int dimensions = sizes.size();
	if (dimensions != (int) origins.size() || dimensions != (int) scales.size())
		throw SourceException("json invalid, different dimensions in data");

	epsg_t epsg = jrm.get("epsg", EPSG_UNKNOWN).asInt();
	if (dimensions == 1) {
		lcrs = new LocalCRS(
			epsg,
			sizes.get((Json::Value::ArrayIndex) 0, -1).asInt(),
			origins.get((Json::Value::ArrayIndex) 0, 0).asInt(),
			scales.get((Json::Value::ArrayIndex) 0, 0).asDouble()
		);
	}
	else if (dimensions == 2) {
		lcrs = new LocalCRS(
			epsg,
			sizes.get((Json::Value::ArrayIndex) 0, -1).asInt(), sizes.get((Json::Value::ArrayIndex) 1, -1).asInt(),
			origins.get((Json::Value::ArrayIndex) 0, 0).asInt(), origins.get((Json::Value::ArrayIndex) 1, 0).asInt(),
			scales.get((Json::Value::ArrayIndex) 0, 0).asDouble(), scales.get((Json::Value::ArrayIndex) 1, 0).asDouble()
		);
	}
	else if (dimensions == 3) {
		lcrs = new LocalCRS(
			epsg,
			sizes.get((Json::Value::ArrayIndex) 0, -1).asInt(), sizes.get((Json::Value::ArrayIndex) 1, -1).asInt(), sizes.get((Json::Value::ArrayIndex) 2, -1).asInt(),
			origins.get((Json::Value::ArrayIndex) 0, 0).asInt(), origins.get((Json::Value::ArrayIndex) 1, 0).asInt(), origins.get((Json::Value::ArrayIndex) 2, 0).asInt(),
			scales.get((Json::Value::ArrayIndex) 0, 0).asDouble(), scales.get((Json::Value::ArrayIndex) 1, 0).asDouble(), scales.get((Json::Value::ArrayIndex) 2, 0).asDouble()
		);
	}
	else
		throw SourceException("json invalid, dimensions not between 1 and 3");

	lcrs->verify();

	Json::Value channelinfo = root["channels"];
	if (!channelinfo.isArray() || channelinfo.size() < 1) {
		printf("No channel information in json\n");
		throw SourceException("No channel information in json");
	}

	channelcount = channelinfo.size();
	channels = new DataDescription *[channelcount];
	for (int i=0;i<channelcount;i++)
		channels[i] = nullptr;

	for (int i=0;i<channelcount;i++) {
		Json::Value channel = channelinfo[(Json::Value::ArrayIndex) i];

		std::string datatype = channel.get("datatype", "unknown").asString();
		bool has_no_data = false;
		double no_data = 0;
		if (channel.isMember("nodata")) {
			has_no_data = true;
			no_data = channel.get("nodata", 0).asDouble();
		}

		channels[i] = new DataDescription(
			GDALGetDataTypeByName(datatype.c_str()),
			channel.get("min", 0).asDouble(),
			channel.get("max", -1).asDouble(),
			has_no_data, no_data
		);
		channels[i]->verify();
	}

	/*
	 * Step #2: open the .db file and initialize it if needed
	 */
	int rc = sqlite3_open(filename_db.c_str(), &db);
	if (rc) {
		printf("Can't open database %s: %s\n", filename_db.c_str(), sqlite3_errmsg(db));
		throw SourceException(sqlite3_errmsg(db));
	}

	dbexec("CREATE TABLE IF NOT EXISTS rasters("
		" id INTEGER PRIMARY KEY,"
		" channel INTEGER NOT NULL,"
		" timestamp INTEGER NOT NULL,"
		" x1 INTEGER NOT NULL,"
		" y1 INTEGER NOT NULL,"
		" z1 INTERGET NOT NULL,"
		" x2 INTEGER NOT NULL,"
		" y2 INTEGER NOT NULL,"
		" z2 INTEGER NOT NULL,"
		" filenr INTEGER NOT NULL,"
		" fileoffset INTEGER NOT NULL,"
		" filebytes INTEGER NOT NULL,"
		" compression INTEGER NOT NULL"
		")"
	);
	dbexec("CREATE UNIQUE INDEX IF NOT EXISTS ctxy ON rasters (channel, timestamp, x1, y1)");
}

void RasterSource::dbexec(const char *query) {
	char *error = 0;
	if (SQLITE_OK != sqlite3_exec(db, query, NULL, NULL, &error)) {
		printf("Error on query %s: %s\n", query, error);
		sqlite3_free(error);
		throw SourceException("db error");
	}
}

void RasterSource::cleanup() {
	if (lockedfile != -1) {
		close(lockedfile); // also removes the lock acquired by flock()
		lockedfile = -1;
	}
	if (lcrs) {
		delete lcrs;
		lcrs = nullptr;
	}
	if (channelcount && channels) {
		for (int i=0;i<channelcount;i++) {
			delete channels[i];
		}
		delete [] channels;
	}

	if (db) {
		sqlite3_close(db);
		db = nullptr;
	}
}



void RasterSource::import(const char *filename, int sourcechannel, int channelid, int timestamp, GenericRaster::Compression compression) {
	if (!isWriteable())
		throw SourceException("Cannot import into a source opened as read-only");
	std::unique_ptr<GenericRaster> raster(
		GenericRaster::fromGDAL(filename, sourcechannel, lcrs->epsg)
	);

	import(raster.get(), channelid, timestamp, compression);
}


void RasterSource::import(GenericRaster *raster, int channelid, int timestamp, GenericRaster::Compression compression) {
	if (!isWriteable())
		throw SourceException("Cannot import into a source opened as read-only");
	if (channelid < 0 || channelid >= channelcount)
		throw SourceException("RasterSource::import: unknown channel");
	if (!(raster->lcrs == *lcrs)) {
		std::cerr << raster->lcrs << std::endl;
		std::cerr << *lcrs << std::endl;
		throw SourceException("RasterMetadata does not match RasterSource");
	}

	if (!(raster->dd == *channels[channelid])) {
		raster->dd.print();
		channels[channelid]->print();
		throw SourceException("ValueMetadata does not match Channel's Metadata");
	}

	std::unique_ptr<ByteBuffer> buffer(
		RasterConverter::direct_encode(raster, compression)
	);

	printf("Method %d, size of data: %ld -> %ld (%f)\n", (int) compression, raster->getDataSize(), buffer->size, (double) buffer->size / raster->getDataSize());

	// Step 1: compress and write
	size_t filenr = 0;

	FILE *f = fopen(filename_data.c_str(), "a+b");
	if (!f)
		throw SourceException("Could not open data file");

	if (fseek(f, 0, SEEK_END) != 0) {
		fclose(f);
		throw SourceException("tell failed");
	}
	long int fileoffset = ftell(f);
	if (fileoffset < 0) {
		fclose(f);
		throw SourceException("tell failed");
	}

	printf("writing to file starting at position %ld\n", fileoffset);
	size_t written = fwrite(buffer->data, sizeof(unsigned char), buffer->size, f);

	fclose(f);
	if (written != buffer->size) {
		throw SourceException("writing failed, disk full?");
	}


	// Step 2: insert into DB
	sqlite3_stmt *stmt = nullptr;

  if (SQLITE_OK != sqlite3_prepare_v2(
			db,
			"INSERT INTO rasters (channel, timestamp, x1, y1, z1, x2, y2, z2, filenr, fileoffset, filebytes, compression) VALUES (?,?,?,?,?,?,?,?,?,?,?,?)",
			-1, // read until '\0'
			&stmt,
			NULL
		)) {
		throw SourceException("Cannot prepare statement");
	}

	if ( SQLITE_OK != sqlite3_bind_int(stmt, 1, channelid)
		|| SQLITE_OK != sqlite3_bind_int(stmt, 2, timestamp)
		|| SQLITE_OK != sqlite3_bind_int(stmt, 3, 0) // x1
		|| SQLITE_OK != sqlite3_bind_int(stmt, 4, 0) // y1
		|| SQLITE_OK != sqlite3_bind_int(stmt, 5, 0) // z1
		|| SQLITE_OK != sqlite3_bind_int(stmt, 6, raster->lcrs.size[0]) // x2
		|| SQLITE_OK != sqlite3_bind_int(stmt, 7, raster->lcrs.size[1]) // y2
		|| SQLITE_OK != sqlite3_bind_int(stmt, 8, raster->lcrs.size[2]) // z2
		|| SQLITE_OK != sqlite3_bind_int(stmt, 9, filenr) // filenr
		|| SQLITE_OK != sqlite3_bind_int64(stmt, 10, fileoffset) // fileoffset
		|| SQLITE_OK != sqlite3_bind_int64(stmt, 11, buffer->size)
		|| SQLITE_OK != sqlite3_bind_int(stmt, 12, compression) // compression method
		) {
		throw SourceException("error binding");
	}

	if (SQLITE_DONE != sqlite3_step(stmt)) {
		throw SourceException("error inserting into DB");
	}

	sqlite3_finalize(stmt);
	stmt = nullptr;
}


GenericRaster *RasterSource::load(int channelid, int timestamp, int x1, int y1, int x2, int y2) {
	if (channelid < 0 || channelid >= channelcount)
		throw SourceException("RasterSource::load: unknown channel");

	Profiler::Profiler p_all("RasterSource::load");
	// TODO: erstmal einen timestamp finden, der in der DB enthalten ist.

/*
	x1 = std::max(x1, 0);
	y1 = std::max(y1, 0);
	x2 = std::min(x2, (int) lcrs->size[0]);
	y2 = std::min(y2, (int) lcrs->size[1]);
*/
	/*
	if (x1 < 0 || y1 < 0 || (size_t) x2 > lcrs->size[0] || (size_t) y2 > lcrs->size[1]) {
		std::stringstream ss;
		ss << "RasterSource::load(" << channelid << ", " << timestamp << ", ["<<x1 <<"," << y1 <<" -> " << x2 << "," << y2 << "]): coords out of bounds";
		throw SourceException(ss.str());
	}
	*/

	if (x1 > x2 || y1 > y2) {
		std::stringstream ss;
		ss << "RasterSource::load(" << channelid << ", " << timestamp << ", ["<<x1 <<"," << y1 <<" -> " << x2 << "," << y2 << "]): coords swapped";
		throw SourceException(ss.str());
	}

	// find all overlapping rasters in DB
	Profiler::start("RasterSource::load: sqlite");
	sqlite3_stmt *stmt = nullptr;

  if (SQLITE_OK != sqlite3_prepare_v2(
			db,
			"SELECT x1,y1,z1,filenr,fileoffset,filebytes,compression FROM rasters WHERE channel = ? AND x1 < ? AND y1 < ? AND x2 >= ? AND y2 >= ? AND timestamp = ?",
			-1, // read until '\0'
			&stmt,
			NULL
		)) {
		throw SourceException("Cannot prepare statement");
	}

	if ( SQLITE_OK != sqlite3_bind_int(stmt, 1, channelid)
		|| SQLITE_OK != sqlite3_bind_int(stmt, 2, x2)
		|| SQLITE_OK != sqlite3_bind_int(stmt, 3, y2)
		|| SQLITE_OK != sqlite3_bind_int(stmt, 4, x1)
		|| SQLITE_OK != sqlite3_bind_int(stmt, 5, y1)
		|| SQLITE_OK != sqlite3_bind_int(stmt, 6, timestamp)
		) {
		throw SourceException("error binding");
	}
	Profiler::stop("RasterSource::load: sqlite");

	Profiler::start("RasterSource::load: create");
	// Create an empty raster of the desired size
	LocalCRS resultmetadata(
		lcrs->epsg,
		x2-x1, y2-y1,
		lcrs->PixelToWorldX(x1), lcrs->PixelToWorldY(y1),
		lcrs->scale[0], lcrs->scale[1]
	);
	std::unique_ptr<GenericRaster> result(
		GenericRaster::create(resultmetadata, *channels[channelid])
	);
	result->clear(channels[channelid]->no_data);
	Profiler::stop("RasterSource::stop: create");

	// Load all overlapping parts and blit them onto the empty raster
	int res, tiles_found = 0;
	while ((res = sqlite3_step(stmt)) != SQLITE_DONE) {
		if (res != SQLITE_ROW)
			throw SourceException("error loading from DB");

		int r_x1 = sqlite3_column_int(stmt, 0);
		int r_y1 = sqlite3_column_int(stmt, 1);
		//int r_z1 = sqlite3_column_int(stmt, 2);
		int fileid = sqlite3_column_int(stmt, 3);
		size_t fileoffset = sqlite3_column_int64(stmt, 4);
		size_t filebytes = sqlite3_column_int64(stmt, 5);
		GenericRaster::Compression method = (GenericRaster::Compression) sqlite3_column_int(stmt, 6);

		//fprintf(stderr, "loading raster from %d,%d, blitting to %d, %d\n", r_x1, r_y1, x1, y1);
		std::unique_ptr<GenericRaster> tmpraster(
			load(channelid, fileid, fileoffset, filebytes, method)
		);
		Profiler::start("RasterSource::load: blit");
		result->blit(tmpraster.get(), r_x1 - x1, r_y1 - y1, 0/*r_z1 - z1*/);
		Profiler::stop("RasterSource::load: blit");
		tiles_found++;
	}

	sqlite3_finalize(stmt);
	stmt = nullptr;

	if (tiles_found == 0)
		throw SourceException("RasterSource::load(): No matching tiles found in DB");

	return result.release();
}


GenericRaster *RasterSource::load(int channelid, int fileid, size_t offset, size_t size, GenericRaster::Compression method) {
	if (channelid < 0 || channelid >= channelcount)
		throw SourceException("RasterSource::load: unknown channel");

	//printf("loading raster from file %d offset %ld length %ld\n", fileid, offset, size);

	Profiler::start("RasterSource::load: File IO");
#define USE_POSIX_IO true
#if USE_POSIX_IO
	int f = open(filename_data.c_str(), O_RDONLY | O_CLOEXEC); // | O_NOATIME
	if (f < 0)
		throw SourceException("Could not open data file");

	ByteBuffer buffer(size);
	if (lseek(f, (off_t) offset, SEEK_SET) != (off_t) offset) {
		close(f);
		throw SourceException("seek failed");
	}

	if (read(f, buffer.data, size) != (ssize_t) size) {
		close(f);
		throw SourceException("read failed");
	}
	close(f);
#else
	FILE *f = fopen(filename_data.c_str(), "rb");
	if (!f)
		throw SourceException("Could not open data file");

	if (fseek(f, offset, SEEK_SET) != 0) {
		fclose(f);
		throw SourceException("seek failed");
	}

	ByteBuffer buffer(size);
	if (fread(buffer.data, sizeof(unsigned char), size, f) != size) {
		fclose(f);
		throw SourceException("read failed");
	}
	fclose(f);
#endif
	Profiler::stop("RasterSource::load: File IO");

	// decode / decompress
	Profiler::Profiler p("RasterSource::load: decompress");
	return RasterConverter::direct_decode(*lcrs, *channels[channelid], &buffer, method);
}





std::unordered_map<std::string, RasterSource *> RasterSourceManager::map;
std::mutex RasterSourceManager::mutex;

RasterSource *RasterSourceManager::open(const char *filename, bool writeable)
{
	std::lock_guard<std::mutex> guard(mutex);

	// To make sure each source is only accessed by a single path, resolve all symlinks
	char filename_clean[PATH_MAX];
	if (realpath(filename, filename_clean) == nullptr)
		throw new SourceException("realpath() failed");

	std::string path(filename_clean);

	RasterSource *source = nullptr;
	if (map.count(path) == 1) {
		source = map.at(path);
	}
	else {
		source = new RasterSource(path.c_str(), writeable);
		map[path] = source;
	}

	if (writeable && !source->isWriteable())
		throw new SourceException("Cannot re-open source as read/write (TODO?)");

	source->refcount++;
	return source;
}

void RasterSourceManager::close(RasterSource *source)
{
	if (!source)
		return;
	std::lock_guard<std::mutex> guard(mutex);

	source->refcount--;
	if (source->refcount <= 0)
		delete source;
}


