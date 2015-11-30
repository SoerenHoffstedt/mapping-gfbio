/*
 * cache_structure.cpp
 *
 *  Created on: 09.08.2015
 *      Author: mika
 */

#include "cache/node/node_cache.h"
#include "cache/index/index_cache.h"

#include "cache/priv/cache_structure.h"
#include "cache/common.h"

#include "util/log.h"
#include "util/concat.h"

#include <iostream>

//
// Key
//

NodeCacheKey::NodeCacheKey(const std::string& semantic_id, uint64_t entry_id) :
	semantic_id(semantic_id), entry_id(entry_id) {
}

NodeCacheKey::NodeCacheKey(BinaryStream& stream) {
	stream.read(&semantic_id);
	stream.read(&entry_id);
}

void NodeCacheKey::toStream(BinaryStream& stream) const {
	stream.write(semantic_id);
	stream.write(entry_id);
}

std::string NodeCacheKey::to_string() const {
	return concat( "NodeCacheKey[ semantic_id: ", semantic_id, ", id: ", entry_id, "]");
}

TypedNodeCacheKey::TypedNodeCacheKey(CacheType type, const std::string& semantic_id, uint64_t entry_id) :
	NodeCacheKey(semantic_id,entry_id), type(type) {
}

TypedNodeCacheKey::TypedNodeCacheKey(BinaryStream& stream) : NodeCacheKey(stream) {
	stream.read(&type);
}

void TypedNodeCacheKey::toStream(BinaryStream& stream) const {
	NodeCacheKey::toStream(stream);
	stream.write(type);
}

std::string TypedNodeCacheKey::to_string() const {
	return concat( "TypedNodeCacheKey[ type: ", (int)type, ", semantic_id: ", semantic_id, ", id: ", entry_id, "]");
}

//
// Bounds
//

ResolutionInfo::ResolutionInfo() :
	restype(QueryResolution::Type::NONE),
	actual_pixel_scale_x(0),
	actual_pixel_scale_y(0) {
}

ResolutionInfo::ResolutionInfo(const GridSpatioTemporalResult& result) :
	restype(QueryResolution::Type::PIXELS),
	pixel_scale_x( result.pixel_scale_x, result.pixel_scale_x*2 ),
	pixel_scale_y( result.pixel_scale_y, result.pixel_scale_y*2 ),
	actual_pixel_scale_x(result.pixel_scale_x),
	actual_pixel_scale_y(result.pixel_scale_y) {
}

ResolutionInfo::ResolutionInfo(BinaryStream& stream) :
	pixel_scale_x(stream), pixel_scale_y(stream) {
	stream.read(&restype);
	stream.read(&actual_pixel_scale_x);
	stream.read(&actual_pixel_scale_y);
}

void ResolutionInfo::toStream(BinaryStream& stream) const {
	pixel_scale_x.toStream(stream);
	pixel_scale_y.toStream(stream);
	stream.write(restype);
	stream.write(actual_pixel_scale_x);
	stream.write(actual_pixel_scale_y);
}

bool ResolutionInfo::matches(const QueryRectangle& query) {
	return query.restype == restype &&
	// No resolution --> matches
    (restype == QueryResolution::Type::NONE ||
    	// Check res
	    (pixel_scale_x.contains((query.x2-query.x1) / query.xres) &&
		 pixel_scale_y.contains((query.y2-query.y1) / query.yres))
    );
}


QueryCube::QueryCube(const QueryRectangle& rect) : QueryCube(rect,rect) {
}

QueryCube::QueryCube(const SpatialReference& sref, const TemporalReference& tref) :
	Cube3( sref.x1, sref.x2, sref.y1, sref.y2, tref.t1,
	// Always make timespan an interval -- otherwise the volume function of cube returns 0
	// Currently only works for unix timestamps
	std::max( tref.t2, tref.t1 + 0.25 ) ), epsg(sref.epsg), timetype(tref.timetype) {
}

QueryCube::QueryCube(BinaryStream& stream) : Cube3(stream) {
	stream.read(&epsg);
	stream.read(&timetype);
}

void QueryCube::toStream(BinaryStream& stream) const {
	Cube3::toStream(stream);
	stream.write(epsg);
	stream.write(timetype);
}

CacheCube::CacheCube(const SpatialReference& sref, const TemporalReference& tref) :
	QueryCube( sref, tref ) {
}

CacheCube::CacheCube(const SpatioTemporalResult& result) : CacheCube( result.stref, result.stref ) {
}

CacheCube::CacheCube(const GridSpatioTemporalResult& result) :
	QueryCube(result.stref,result.stref), resolution_info(result) {
//	double ohspan = result.stref.x2 - result.stref.x1;
//	double ovspan = result.stref.y2 - result.stref.y1;
//
//	// Enlarge result by degrees of 1/100 a pixel in each direction
//	double h_spacing = ohspan / result.width / 100.0;
//	double v_spacing = ovspan / result.height / 100.0;
//
//	double x1 = result.stref.x1 - h_spacing;
//	double x2 = result.stref.x2 + h_spacing;
//	double y1 = result.stref.y1 - v_spacing;
//	double y2 = result.stref.y2 + v_spacing;
//
//	set_dimension( 0, x1, x2 );
//	set_dimension( 1, y1, y2 );
}

CacheCube::CacheCube(const GenericPlot& result) :
	QueryCube( SpatialReference(EPSG_UNREFERENCED,DoubleNegInfinity,DoubleNegInfinity,DoubleInfinity,DoubleInfinity),
		       TemporalReference(TIMETYPE_UNREFERENCED, DoubleNegInfinity,DoubleInfinity) ) {
	(void) result;
}


CacheCube::CacheCube(BinaryStream& stream) : QueryCube(stream), resolution_info(stream) {
}

const Interval& CacheCube::get_timespan() const {
	return get_dimension(2);
}

void CacheCube::toStream(BinaryStream& stream) const {
	QueryCube::toStream(stream);
	resolution_info.toStream(stream);
}

//
// AccessInfo
//

AccessInfo::AccessInfo() : last_access(time(nullptr)), access_count(1) {
}

AccessInfo::AccessInfo( time_t last_access, uint32_t access_count ) :
	last_access(last_access), access_count(access_count) {
}

AccessInfo::AccessInfo( BinaryStream &stream ) {
	stream.read(&last_access);
	stream.read(&access_count);
}

void AccessInfo::toStream( BinaryStream &stream ) const {
	stream.write(last_access);
	stream.write(access_count);
}

//
// MoveInfo
//
MoveInfo::MoveInfo(uint64_t size, double costs) : size(size), costs(costs) {
}

MoveInfo::MoveInfo(time_t last_access, uint32_t access_count, uint64_t size, double costs) :
	AccessInfo(last_access,access_count), size(size), costs(costs) {
}

MoveInfo::MoveInfo(BinaryStream& stream) : AccessInfo(stream) {
	stream.read(&size);
	stream.read(&costs);
}

void MoveInfo::toStream(BinaryStream& stream) const {
	AccessInfo::toStream(stream);
	stream.write(size);
	stream.write(costs);
}

//
// CacheEntry
//
CacheEntry::CacheEntry(CacheCube bounds, uint64_t size, double costs) : MoveInfo(size, costs),
	bounds(bounds) {
}

CacheEntry::CacheEntry(CacheCube bounds, uint64_t size, double costs, time_t last_access, uint32_t access_count) :
	MoveInfo(last_access,access_count,size,costs),
	bounds(bounds) {
}

CacheEntry::CacheEntry(BinaryStream& stream) : MoveInfo(stream), bounds(stream) {
}

void CacheEntry::toStream(BinaryStream& stream) const {
	MoveInfo::toStream(stream);
	bounds.toStream(stream);
}

std::string CacheEntry::to_string() const {
	return concat("CacheEntry[size: ", size, ",costs: ", costs, ", last_access: ", last_access, ", access_count: ", access_count, ", bounds: ", bounds.to_string(), "]");
}

//
// Query Info
//

template<typename KType>
CacheQueryInfo<KType>::CacheQueryInfo(double score, CacheCube cube, KType key) :
	score(score), cube(cube), key(key) {
}

template<typename KType>
bool CacheQueryInfo<KType>::operator <(const CacheQueryInfo& b) const {
	return score < b.score;
}

template<typename KType>
std::string CacheQueryInfo<KType>::to_string() const {
	return concat("CacheQueryInfo: ", cube, ", score: ", score);
}

//
// Query Result
//
template<typename KType>
CacheQueryResult<KType>::CacheQueryResult(const QueryRectangle& query) :
	covered(query) {
	remainder.push_back( Cube3(query.x1,query.x2,query.y1,query.y2,query.t1,query.t2) );
}

template<typename KType>
CacheQueryResult<KType>::CacheQueryResult( const QueryRectangle &query, std::vector<Cube<3>> remainder, std::vector<KType> keys) :
	covered(query),
	keys(keys),
	remainder( remainder ) {

	QueryCube qc(query);
	double v = 0;
	for ( auto &rem : this->remainder) {
		v += rem.volume();
	}
}

template<typename KType>
bool CacheQueryResult<KType>::has_remainder() const {
	return !remainder.empty();
}

template<typename KType>
bool CacheQueryResult<KType>::has_hit() const {
	return !keys.empty();
}

template<typename KType>
std::string CacheQueryResult<KType>::to_string() const {
	return concat( "CacheQueryResult[",
	"has_hit: ", has_hit(),
	",  has_remainder: ", has_remainder(),
	",  num remainders: ", remainder.size(),
	",  num keys: ", keys.size(),
	"]");
}


//
// CacheRef
//

NodeCacheRef::NodeCacheRef(const TypedNodeCacheKey& key, const CacheEntry& entry) :
	TypedNodeCacheKey(key), CacheEntry(entry) {
}

NodeCacheRef::NodeCacheRef(CacheType type, const NodeCacheKey& key, const CacheEntry& entry) :
	TypedNodeCacheKey(type,key.semantic_id,key.entry_id), CacheEntry(entry) {
}

NodeCacheRef::NodeCacheRef(CacheType type, const std::string semantic_id, uint64_t entry_id, const CacheEntry& entry) :
	TypedNodeCacheKey(type,semantic_id,entry_id), CacheEntry(entry) {
}

NodeCacheRef::NodeCacheRef(BinaryStream& stream) :
	TypedNodeCacheKey(stream), CacheEntry(stream) {
}

void NodeCacheRef::toStream(BinaryStream& stream) const {
	TypedNodeCacheKey::toStream(stream);
	CacheEntry::toStream(stream);
}

std::string NodeCacheRef::to_string() const {
	return concat("CacheRef[ key: ", TypedNodeCacheKey::to_string(), ", entry: ", CacheEntry::to_string(), "]");
}

//////////////////////////////////////////////////////////////
//
// Structure
//
//////////////////////////////////////////////////////////////


template<typename KType, typename EType>
void CacheStructure<KType, EType>::put(const KType& key, const std::shared_ptr<EType>& result) {
	std::lock_guard<std::mutex> guard(mtx);
//	Log::trace("Inserting new entry. Id: %d", key );
	entries.emplace(key, result);
}

template<typename KType, typename EType>
std::shared_ptr<EType> CacheStructure<KType, EType>::get(const KType& key) const {
	std::lock_guard<std::mutex> guard(mtx);
	try {
		return entries.at(key);
	} catch (const std::out_of_range &oor) {
		throw NoSuchElementException("No cache-entry found");
	}
}

template<typename KType, typename EType>
std::shared_ptr<EType> CacheStructure<KType, EType>::remove(const KType& key) {
	std::lock_guard<std::mutex> guard(mtx);
	auto iter = entries.find(key);
	if ( iter != entries.end() ) {
		auto result = iter->second;
		entries.erase(iter);
		return result;
	}
	throw NoSuchElementException("No cache-entry found");
}

//
// QUERY STUFF
//

template<typename KType, typename EType>
std::priority_queue<CacheQueryInfo<KType>> CacheStructure<KType, EType>::get_query_candidates(
	const QueryRectangle& spec) const {

	Log::trace("Fetching candidates for query: %s", CacheCommon::qr_to_string(spec).c_str() );
	std::priority_queue<CacheQueryInfo<KType>> partials;

	QueryCube qc( spec );

	for (auto &e : entries) {
		CacheCube &bounds = e.second->bounds;

		if ( spec.epsg == bounds.epsg &&
			 spec.timetype == bounds.timetype &&
			 bounds.resolution_info.matches(spec) &&
			 bounds.intersects(qc) ) {

			// Raster
			if ( spec.restype == QueryResolution::Type::PIXELS &&
				!bounds.get_timespan().contains( Interval( spec.t1, spec.t2) ) )
				continue;

			// Coverage = score for now
			double score = bounds.intersect(qc).volume() / qc.volume();
			Log::trace("Score for entry %s: %f", key_to_string(e.first).c_str(), score);
			partials.push(CacheQueryInfo<KType>( score, bounds, e.first ));
		}
	}
	Log::trace("Found %d candidates for query: %s", partials.size(), CacheCommon::qr_to_string(spec).c_str() );
	return partials;
}

template<typename KType, typename EType>
const CacheQueryResult<KType> CacheStructure<KType, EType>::query(const QueryRectangle& spec) const {
	std::lock_guard<std::mutex> guard(mtx);

	Log::trace("Querying cache for: %s", CacheCommon::qr_to_string(spec).c_str() );

	// Get intersecting entries
	std::priority_queue<CacheQueryInfo<KType>> partials = get_query_candidates( spec );

	// No candidates found
	if ( partials.empty() ) {
		Log::trace("No candidates cached.");
		return CacheQueryResult<KType>(spec);
	}

	//std::vector<KType> ids;
	std::vector<CacheQueryInfo<KType>> used_entries;
	std::vector<Cube<3>> remainders;
	remainders.push_back( QueryCube(spec) );

	while ( !partials.empty() && !remainders.empty() ) {
		bool used   = false;
		const CacheQueryInfo<KType> &entry = partials.top();

		// Skip incompatible resolutions
		if ( spec.restype == QueryResolution::Type::PIXELS &&
			 !used_entries.empty() &&
			 !CacheCommon::resolution_matches(
				 entry.cube, used_entries.front().cube)	) {
			partials.pop();
			continue;
		}

		std::vector<Cube<3>> new_remainders;
		for ( auto & rem : remainders ) {
			if ( entry.cube.intersects(rem) ) {
				used = true;
				auto split = rem.dissect_by(entry.cube);
				// Insert new remainders
				if ( !split.empty() )
					new_remainders.insert(new_remainders.end(),split.begin(),split.end());
			}
			else
				new_remainders.push_back(rem);
		}
		remainders = new_remainders;

		if ( used ) {
			used_entries.push_back(entry);
			//ids.push_back( entry.key );
		}
		partials.pop();
	}

	std::vector<Cube<3>> u_rems = union_remainders(remainders);
	if ( u_rems.size() != remainders.size() )
		Log::trace("Union produced %ld instead of %ld remainders", u_rems.size(), remainders.size());
	else
		Log::trace("Union had no effect, remainders: %ld", remainders.size() );


	return enlarge_expected_result(spec, used_entries, u_rems);
}

template<typename KType, typename EType>
std::vector<Cube<3> > CacheStructure<KType, EType>::union_remainders(
		const std::vector<Cube<3> >& remainders) const {

	std::vector<Cube<3>> result;
	std::vector<Cube<3>> work = remainders;

	while ( !work.empty() ) {
		// Take one cube
		auto current = work.back();
		work.pop_back();

		// See if it can be combined with any of the remaining cubes
		// If so: start over since we maybe can add more now
		auto i = work.begin();
		while ( i != work.end() ) {
			auto tmp = current.combine(*i);
			if ( tmp.volume() < (current.volume() + i->volume()) * 1.01 ) {
				current = tmp;
				work.erase(i);
				i = work.begin();
			}
			else
				i++;
		}
		result.push_back(current);
	}
	return result;
}

template<typename KType, typename EType>
CacheQueryResult<KType> CacheStructure<KType, EType>::enlarge_expected_result(
		const QueryRectangle& orig, const std::vector<CacheQueryInfo<KType>>& hits,
		std::vector<Cube<3> >& remainders) const {

	// Extract ids for the result;
	std::vector<KType> ids;

	// Tells which dimensions may be extended
	bool do_check[6] = {true,true,true,true,true,true};

	const QueryCube qc(orig);
	double rem_volume = 0;
	// If we have a raster, we extend it in spatial dimensions and
	// enlarge time-interval to t1 = max(hits.t1) and t2 = min(hits.t2)
	// since there MUST NOT be two results with different time-intervals
	int check_dims = (orig.restype == QueryResolution::Type::PIXELS) ? 2 : 3;

	// Calculate coverage and check which edges may be extended
	// Only extend edges untouched by a remainder
	for ( auto &rem : remainders ) {
		rem_volume += rem.volume();
		for ( int i = 0; i < check_dims; i++ ) {
			auto &rdim = rem.get_dimension(i);
			auto &qdim = qc.get_dimension(i);
			// Extend if it not touches to borders of the query
			do_check[2*i] &= rdim.a > qdim.a;
			do_check[2*i+1] &= rdim.b < qdim.b;
		}
	}

	// Return miss if we have a low coverage (<10%)
	if ( rem_volume/qc.volume() > 0.9 )
		return CacheQueryResult<KType>( orig );


	// Calculated maximum covered cube
	double values[6] = { do_check[0] ? DoubleNegInfinity : orig.x1,
				   	     do_check[1] ? DoubleInfinity : orig.x2,
				   	     do_check[2] ? DoubleNegInfinity : orig.y1,
				   	     do_check[3] ? DoubleInfinity : orig.y2,
				   	     do_check[4] ? DoubleNegInfinity : orig.t1,
				   	     do_check[5] ? DoubleInfinity : orig.t2 };



	for ( auto &cqi : hits ) {
		ids.push_back( cqi.key );
		for ( int i = 0; i < 3; i++ ) {
			auto &cdim = cqi.cube.get_dimension(i);
			auto &qdim = qc.get_dimension(i);
			int idx_l = 2*i;
			int idx_r = idx_l + 1;

			// If this item touches the bounds of the query.... extend
			if ( do_check[idx_l] && cdim.a <= qdim.a )
				values[idx_l] = std::max(values[idx_l],cdim.a);

			if ( do_check[idx_r] && cdim.b >= qdim.b )
				values[idx_r] = std::min(values[idx_r],cdim.b);
		}
	}


	// Final check... stupid floating point stuff
	for ( int i = 0; i < 6; i++ ) {
		if ( !std::isfinite(values[i]) ) {
			auto &d = qc.get_dimension(i/2);
			values[i] = (i%2) == 0 ? d.a : d.b;
		}
	}


	QueryResolution qr = QueryResolution::none();

	// RASTER ONLY
	// Set time on remainders and calculate resolution
	if ( orig.restype == QueryResolution::Type::PIXELS ) {
		for ( auto &rem : remainders )
			rem.set_dimension(2,values[4],values[5]);
		int w = std::ceil(orig.xres / (orig.x2-orig.x1) * (values[1]-values[0]));
		int h = std::ceil(orig.yres / (orig.y2-orig.y1) * (values[3]-values[2]));
		qr = QueryResolution::pixels(w,h);
	}

	QueryRectangle new_query(
		SpatialReference( orig.epsg, values[0], values[2], values[1], values[3] ),
		TemporalReference( orig.timetype, values[4], values[5] ),
		qr
	);

	Log::trace("Extended Query:\norig: %s\nnew : %s", CacheCommon::qr_to_string(orig).c_str(),CacheCommon::qr_to_string(new_query).c_str());
	return CacheQueryResult<KType>( new_query, remainders, ids );
}


//
// END QUERY STUFF
//



template<typename KType, typename EType>
std::vector<std::shared_ptr<EType> > CacheStructure<KType, EType>::get_all() const {
	std::vector<std::shared_ptr<EType> > result;
	for (auto &e : entries) {
		result.push_back(e.second);
	}
	return result;
}

template<typename KType, typename EType>
std::string CacheStructure<KType, EType>::key_to_string(uint64_t key) const {
	return std::to_string(key);
}

template<typename KType, typename EType>
std::string CacheStructure<KType, EType>::key_to_string(const std::pair<uint32_t, uint64_t> &key) const {
	return concat("(",key.first,":",key.second,")");
}

template class CacheQueryResult<uint64_t>;
template class CacheStructure<uint64_t, NodeCacheEntry<GenericRaster>>;
template class CacheStructure<uint64_t, NodeCacheEntry<PointCollection>>;
template class CacheStructure<uint64_t, NodeCacheEntry<LineCollection>>;
template class CacheStructure<uint64_t, NodeCacheEntry<PolygonCollection>>;
template class CacheStructure<uint64_t, NodeCacheEntry<GenericPlot>>;

template class CacheQueryResult<std::pair<uint32_t,uint64_t>>;
template class CacheStructure<std::pair<uint32_t, uint64_t>, IndexCacheEntry> ;
