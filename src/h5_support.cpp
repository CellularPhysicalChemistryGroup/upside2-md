#include "h5_support.h"


namespace h5 {

int h5_noerr(int i) {
    if(i<0) {
        // H5Eprint2(H5E_DEFAULT, stderr);
        throw std::string("error " + std::to_string(i));
    }
    return i;
}

int h5_noerr(const char* nm, int i) {
    if(i<0) {
        // H5Eprint2(H5E_DEFAULT, stderr);
        throw std::string("error " + std::to_string(i) + " in " + nm);
    }
    return i;
}

H5Obj h5_obj(H5DeleterFunc &deleter, hid_t obj)
{
    return H5Obj(h5_noerr(obj), H5Deleter(&deleter));
}

std::vector<hsize_t> get_dset_size(int ndims, hid_t group, const char* name) 
try {
    std::vector<hsize_t> ret(ndims, 0);
    auto dset  = h5_obj(H5Dclose, H5Dopen2(group, name, H5P_DEFAULT));
    auto space = h5_obj(H5Sclose, H5Dget_space(dset.get()));

    int ndims_actual = h5_noerr(H5Sget_simple_extent_ndims(space.get()));
    if(ndims_actual != ndims) 
        throw std::string("wrong number of dimensions (expected ") + 
            std::to_string(ndims) + ", but got " + std::to_string(ndims_actual) + ")";
    h5_noerr("H5Sget_simple_extent_dims", H5Sget_simple_extent_dims(space.get(), ret.data(), NULL));
    return ret;
} catch(const std::string &e) {
    throw "while getting size of '" + std::string(name) + "', " + e;
}


bool h5_exists(hid_t base, const char* nm, bool check_valid) {
    return h5_noerr(H5LTpath_valid(base, nm, check_valid));
}

template <>
std::vector<std::string> read_attribute<std::vector<std::string>>(hid_t h5, const char* path, const char* attr_name) 
try {
    auto attr  = h5_obj(H5Aclose, H5Aopen_by_name(h5, path, attr_name, H5P_DEFAULT, H5P_DEFAULT));
    auto space = h5_obj(H5Sclose, H5Aget_space(attr.get()));
    auto dtype = h5_obj(H5Tclose, H5Aget_type (attr.get()));
    if(H5Tis_variable_str(dtype.get())) throw std::string("variable-length strings not supported");

    size_t maxchars = H5Tget_size(dtype.get());
    if(maxchars==0) throw std::string("H5Tget_size error"); // defined error value

    if(H5Sget_simple_extent_ndims(space.get()) != 1) throw std::string("wrong size for attribute");
    hsize_t dims[1]; h5_noerr(H5Sget_simple_extent_dims(space.get(), dims, NULL));

    auto tmp = std::unique_ptr<char>(new char[dims[0]*maxchars+1]);
    std::fill(tmp.get(), tmp.get()+dims[0]*maxchars+1, '\0');
    h5_noerr(H5Aread(attr.get(), dtype.get(), tmp.get()));

    std::vector<std::string> ret;

    auto g = [&](int i, char& s) {
        std::string tmp(maxchars,'\0');
        std::copy(&s, &s+maxchars, begin(tmp));
        while(tmp.size() && tmp.back() == '\0') tmp.pop_back();
        ret.push_back(tmp);
    };
    traverse_dataset_iteraction_helper<1,char,decltype(g)>()(tmp.get(), dims, g, maxchars);
    return ret;
} 
catch(const std::string &e) {
    throw "while reading attribute '" + std::string(attr_name) + "' of '" + std::string(path) + "', " + e;
}

void check_size(hid_t group, const char* name, std::vector<size_t> sz)
{
    size_t ndim = sz.size();
    auto dims = get_dset_size(ndim, group, name);
    for(size_t d=0; d<ndim; ++d) {
        if(dims[d] != sz[d]) {
            std::string msg = std::string("dimensions of '") + name + std::string("', expected (");
            for(size_t i=0; i<ndim; ++i) msg += std::to_string(sz  [i]) + std::string((i<ndim-1) ? ", " : "");
            msg += ") but got (";
            for(size_t i=0; i<ndim; ++i) msg += std::to_string(dims[i]) + std::string((i<ndim-1) ? ", " : "");
            msg += ")";
            throw msg;
        }
    }
}

void check_size(hid_t group, const char* name, size_t sz) 
{ check_size(group, name, std::vector<size_t>(1,sz)); }

void check_size(hid_t group, const char* name, size_t sz1, size_t sz2) 
{ check_size(group, name, std::vector<size_t>{{sz1,sz2}}); }

void check_size(hid_t group, const char* name, size_t sz1, size_t sz2, size_t sz3) 
{ check_size(group, name, std::vector<size_t>{{sz1,sz2,sz3}}); }

void check_size(hid_t group, const char* name, size_t sz1, size_t sz2, size_t sz3, size_t sz4) 
{ check_size(group, name, std::vector<size_t>{{sz1,sz2,sz3,sz4}}); }

void check_size(hid_t group, const char* name, size_t sz1, size_t sz2, size_t sz3, size_t sz4, size_t sz5) 
{ check_size(group, name, std::vector<size_t>{{sz1,sz2,sz3,sz4,sz5}}); }


H5Obj ensure_group(hid_t loc, const char* nm) {
    return h5_obj(H5Gclose, h5_exists(loc, nm) 
            ? H5Gopen2(loc, nm, H5P_DEFAULT)
            : H5Gcreate2(loc, nm, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
}

H5Obj open_group(hid_t loc, const char* nm) {
    return h5_obj(H5Gclose, H5Gopen2(loc, nm, H5P_DEFAULT));
}

void ensure_not_exist(hid_t loc, const char* nm) {
    if(h5_exists(loc, nm)) H5Ldelete(loc, nm, H5P_DEFAULT);
}

H5Obj create_earray(hid_t group, const char* name, hid_t dtype,
        const std::initializer_list<int> &dims, // any direction that is extendable must have dims == 0
        const std::initializer_list<int> &chunk_dims,
        bool compression_level)  // 1 is often recommended
{
    if(dims.size() != chunk_dims.size()) throw std::string("invalid chunk dims");

    hsize_t ndims = dims.size();
    std::vector<hsize_t> dims_v(ndims);
    std::vector<hsize_t> max_dims_v(ndims);
    std::vector<hsize_t> chunk_dims_v(ndims);

    for(size_t d=0; d<ndims; ++d) {
        dims_v[d]       = *(begin(dims      )+d);
        max_dims_v[d]   = dims_v[d] ? dims_v[d] : H5S_UNLIMITED;
        chunk_dims_v[d] = *(begin(chunk_dims)+d);
    }

    auto space_id = h5_obj(H5Sclose, H5Screate_simple(ndims, dims_v.data(), max_dims_v.data()));

    // setup chunked, possibly compressed storage
    auto dcpl_id = h5_obj(H5Pclose, H5Pcreate(H5P_DATASET_CREATE));
    h5_noerr(H5Pset_chunk(dcpl_id.get(), ndims, chunk_dims_v.data()));
    h5_noerr(H5Pset_shuffle(dcpl_id.get()));     // improves data compression
    h5_noerr(H5Pset_fletcher32(dcpl_id.get()));  // for verifying data integrity
    if(compression_level) h5_noerr(H5Pset_deflate(dcpl_id.get(), compression_level));

    return h5_obj(H5Dclose, H5Dcreate2(group, name, dtype, space_id.get(), 
                H5P_DEFAULT, dcpl_id.get(), H5P_DEFAULT));
}


}
