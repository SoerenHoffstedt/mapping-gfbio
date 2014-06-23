
#include "raster/raster.h"
#include "raster/typejuggling.h"
#include "raster/profiler.h"
#include "raster/opencl.h"
#include "operators/operator.h"

#include <memory>
#include <cmath>
#include <json/json.h>


class MatrixKernelOperator : public GenericOperator {
	public:
		MatrixKernelOperator(int sourcecount, GenericOperator *sources[], Json::Value &params);
		virtual ~MatrixKernelOperator();

		virtual GenericRaster *getRaster(const QueryRectangle &rect);
	private:
		int matrixsize, *matrix;
};



MatrixKernelOperator::MatrixKernelOperator(int sourcecount, GenericOperator *sources[], Json::Value &params) : GenericOperator(Type::RASTER, sourcecount, sources), matrixsize(0), matrix(nullptr) {
	assumeSources(1);

	matrixsize = params.get("matrix_size", 0).asInt();
	if (matrixsize <= 1 || matrixsize % 2 != 1)
		throw OperatorException("MatrixKernel: kernel size must be odd and greater than 1");

	Json::Value array = params["matrix"];
	size_t matrix_count = (size_t) matrixsize*matrixsize;
	if (array.size() != matrix_count)
		throw OperatorException("MatrixKernel: matrix array has the wrong length");

	matrix = new int[matrix_count];
	for (size_t i=0;i<matrix_count;i++) {
		matrix[i] = array.get((Json::Value::ArrayIndex) i, 0).asInt();
	}
}

MatrixKernelOperator::~MatrixKernelOperator() {
	delete [] matrix;
	matrix = nullptr;
}
REGISTER_OPERATOR(MatrixKernelOperator, "matrix");

template<typename T> T cap(T v, T min, T max) {
	return std::min(max, std::max(v, min));
}

template<typename T>
struct matrixkernel{
	static GenericRaster *execute(Raster2D<T> *raster_src, int matrix_size, int *matrix) {
		std::unique_ptr<GenericRaster> raster_src_guard(raster_src);

		Raster2D<T> *raster_dest = (Raster2D<T> *) GenericRaster::create(raster_src->lcrs, raster_src->dd);
		std::unique_ptr<GenericRaster> raster_dest_guard(raster_dest);
		raster_src->setRepresentation(GenericRaster::Representation::CPU);
		raster_dest->setRepresentation(GenericRaster::Representation::CPU);

		T max = (T) raster_src->valuemeta.max;
		T min = (T) raster_src->valuemeta.min;

		int matrix_offset = matrix_size / 2;
		int width = raster_src->rastermeta.size[0];
		int height = raster_src->rastermeta.size[1];

		// TODO: Rand getrennt verarbeiten, in der mitte ist kein cap nötig
		for (int y=0;y<height;y++) {
			for (int x=0;x<width;x++) {
				typename RasterTypeInfo<T>::accumulator_type value = 0;
				for (int ky=0;ky<matrix_size;ky++) {
					for (int kx=0;kx<matrix_size;kx++) {
						int source_x = cap(x+kx-matrix_offset, 0, width-1);
						int source_y = cap(y+ky-matrix_offset, 0, height-1);

						value += matrix[ky*matrix_size+kx] * raster_src->get(source_x, source_y);
					}
				}
				if (value > max) value = max;
				if (value < min) value = min;
				raster_dest->set(x, y, value);
			}
		}

		return raster_dest_guard.release();
	}
};

#include "operators/raster/matrixkernel.cl.h"

GenericRaster *MatrixKernelOperator::getRaster(const QueryRectangle &rect) {
	GenericRaster *raster = sources[0]->getRaster(rect);

#if 1
	RasterOpenCL::init();
	Profiler::Profiler p("MATRIXKERNEL_CL_OPERATOR");
	raster->setRepresentation(GenericRaster::Representation::OPENCL);

	GenericRaster *raster_out = GenericRaster::create(raster->lcrs, raster->dd, GenericRaster::Representation::OPENCL);
	std::unique_ptr<GenericRaster> raster_out_guard(raster_out);

	size_t matrix_count = (size_t) matrixsize*matrixsize;
	size_t matrix_buffer_size = sizeof(matrix[0]) * matrix_count;

	try {
		RasterOpenCL::CLProgram prog;
		prog.addInRaster(raster);
		prog.addOutRaster(raster_out);
		prog.compile(operators_raster_matrixkernel, "matrixkernel");
		prog.addArg((cl_int) matrixsize);

		cl::Buffer matrixbuffer(
			*RasterOpenCL::getContext(),
			CL_MEM_READ_ONLY,
			matrix_buffer_size,
			nullptr //data
		);
		RasterOpenCL::getQueue()->enqueueWriteBuffer(matrixbuffer, CL_TRUE, 0, matrix_buffer_size, matrix);
		prog.addArg(matrixbuffer);
		prog.run();

	}
	catch (cl::Error &e) {
		printf("cl::Error %d: %s\n", e.err(), e.what());
		throw;
	}

	return raster_out_guard.release();

#else
	Profiler::Profiler p("MATRIXKERNEL_OPERATOR");
	return callUnaryOperatorFunc<matrixkernel>(raster, matrixsize, matrix);
#endif
}

