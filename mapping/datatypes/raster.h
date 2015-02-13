#ifndef RASTER_RASTER_H
#define RASTER_RASTER_H

#include <stdint.h>
#include <gdal_priv.h>

#include <cmath>
#include <ostream>
#include <memory>

#include "raster/exceptions.h"
#include "datatypes/spatiotemporal.h"
#include "datatypes/attributes.h"


class QueryRectangle;
class BinaryStream;


class DataDescription {
	public:
		DataDescription(GDALDataType datatype, double min, double max)
			: datatype(datatype), min(min), max(max), has_no_data(false), no_data(0.0) {};

		DataDescription(GDALDataType datatype, double min, double max, bool has_no_data, double no_data)
			: datatype(datatype), min(min), max(max), has_no_data(has_no_data), no_data(has_no_data ? no_data : 0.0) {};

		DataDescription(BinaryStream &stream);

		DataDescription() = delete;
		~DataDescription() = default;
		// Copy
		DataDescription(const DataDescription &dd) = default;
		DataDescription &operator=(const DataDescription &dd) = default;
		// Move
		DataDescription(DataDescription &&dd) = default;
		DataDescription &operator=(DataDescription &&dd) = default;

		void addNoData();

		bool operator==(const DataDescription &b) const;
		bool operator!=(const DataDescription &b) const { return !(*this == b); }
		void verify() const;

		void toStream(BinaryStream &stream) const;

		int getBPP() const; // Bytes per Pixel
		double getMinByDatatype() const;
		double getMaxByDatatype() const;

		template<typename T> bool is_no_data(T val) const {
			return (has_no_data && val == (T) no_data);
		}
		bool is_no_data(float val) const {
			if (!has_no_data)
				return false;
			if (std::isnan(no_data) && std::isnan(val))
				return true;
			return (val == (float) no_data);
		}
		bool is_no_data(double val) const {
			if (!has_no_data)
				return false;
			if (std::isnan(no_data) && std::isnan(val))
				return true;
			return (val == no_data);
		}

		friend std::ostream& operator<< (std::ostream &out, const DataDescription &dd);

		GDALDataType datatype;
		double min, max;
		bool has_no_data;
		double no_data;
};

namespace cl {
	class Buffer;
}

class Colorizer;
class QueryRectangle;

template<typename T> class Raster2D;

class GenericRaster : public GridSpatioTemporalResult {
	public:
		enum Representation {
			CPU = 1,
			OPENCL = 2
		};

		enum Compression {
			UNCOMPRESSED = 1,
			BZIP = 2,
			PREDICTED = 3,
			GZIP = 4
		};

		virtual void setRepresentation(Representation r) = 0;
		Representation getRepresentation() const { return representation; }

		static std::unique_ptr<GenericRaster> create(const DataDescription &datadescription, const SpatioTemporalReference &stref, uint32_t width, uint32_t height, uint32_t depth = 0, Representation representation = Representation::CPU);
		static std::unique_ptr<GenericRaster> create(const DataDescription &datadescription, const GridSpatioTemporalResult &other, Representation representation = Representation::CPU) {
			return create(datadescription, other.stref, other.width, other.height, 0, representation);
		}
		static std::unique_ptr<GenericRaster> fromGDAL(const char *filename, int rasterid, epsg_t epsg = EPSG_UNKNOWN);
		static std::unique_ptr<GenericRaster> fromGDAL(const char *filename, int rasterid, bool &flipx, bool &flipy, epsg_t epsg = EPSG_UNKNOWN);

		virtual ~GenericRaster();
		GenericRaster(const GenericRaster &) = delete;
		GenericRaster &operator=(const GenericRaster &) = delete;

		void toStream(BinaryStream &stream);
		static std::unique_ptr<GenericRaster> fromStream(BinaryStream &stream);

		virtual void toPGM(const char *filename, bool avg = false) = 0;
		virtual void toYUV(const char *filename) = 0;
		virtual void toPNG(const char *filename, const Colorizer &colorizer, bool flipx = false, bool flipy = false, Raster2D<uint8_t> *overlay = nullptr) = 0;
		virtual void toJPEG(const char *filename, const Colorizer &colorizer, bool flipx = false, bool flipy = false) = 0;
		virtual void toGDAL(const char *filename, const char *driver, bool flipx = false, bool flipy = false) = 0;

		virtual const void *getData() = 0;
		virtual size_t getDataSize() = 0;
		virtual cl::Buffer *getCLBuffer() = 0;
		virtual cl::Buffer *getCLInfoBuffer() = 0;
		virtual void *getDataForWriting() = 0;
		virtual int getBPP() = 0; // Bytes per Pixel
		virtual double getAsDouble(int x, int y=0, int z=0) const = 0;

		virtual void clear(double value) = 0;
		virtual void blit(const GenericRaster *raster, int x, int y=0, int z=0) = 0;
		virtual std::unique_ptr<GenericRaster> cut(int x, int y, int z, int width, int height, int depths) = 0;
		std::unique_ptr<GenericRaster> cut(int x, int y, int width, int height) { return cut(x,y,0,width,height,0); }
		virtual std::unique_ptr<GenericRaster> scale(int width, int height=0, int depth=0) = 0;
		virtual std::unique_ptr<GenericRaster> flip(bool flipx, bool flipy) = 0;
		virtual std::unique_ptr<GenericRaster> fitToQueryRectangle(const QueryRectangle &qrect) = 0;

		virtual void print(int x, int y, double value, const char *text, int maxlen = -1) = 0;
		virtual void printCentered(double value, const char *text);

		std::string hash();

		const DataDescription dd;

		DirectMetadata<std::string> md_string;
		DirectMetadata<double> md_value;

	protected:
		GenericRaster(const DataDescription &datadescription, const SpatioTemporalReference &stref, uint32_t width, uint32_t height, uint32_t depth = 0);
		Representation representation;
};

#endif
