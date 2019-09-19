// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include <faiss/IndexFlat.h>
#include <faiss/IndexIVF.h>
#include <faiss/IndexIVFFlat.h>
#include <faiss/IndexIVFPQ.h>
#include <faiss/AutoTune.h>
#include <faiss/IVFlib.h>
#include <faiss/AuxIndexStructures.h>
#include <faiss/index_io.h>
#include <faiss/gpu/GpuAutoTune.h>


#include "knowhere/common/Exception.h"
#include "knowhere/adapter/VectorAdapter.h"
#include "IndexIVF.h"
#include "IndexGPUIVF.h"


namespace zilliz {
namespace knowhere {


IndexModelPtr IVF::Train(const DatasetPtr &dataset, const Config &config) {
    auto nlist = config["nlist"].as<size_t>();
    auto metric_type = config["metric_type"].as_string() == "L2" ?
                       faiss::METRIC_L2 : faiss::METRIC_INNER_PRODUCT;

    GETTENSOR(dataset)

    faiss::Index *coarse_quantizer = new faiss::IndexFlatL2(dim);
    auto index = std::make_shared<faiss::IndexIVFFlat>(coarse_quantizer, dim, nlist, metric_type);
    index->train(rows, (float *) p_data);

    // TODO: override here. train return model or not.
    return std::make_shared<IVFIndexModel>(index);
}


void IVF::Add(const DatasetPtr &dataset, const Config &config) {
    if (!index_ || !index_->is_trained) {
        KNOWHERE_THROW_MSG("index not initialize or trained");
    }

    std::lock_guard<std::mutex> lk(mutex_);
    GETTENSOR(dataset)

    // TODO: magic here.
    auto array = dataset->array()[0];
    auto p_ids = array->data()->GetValues<long>(1, 0);
    index_->add_with_ids(rows, (float *) p_data, p_ids);
}

void IVF::AddWithoutIds(const DatasetPtr &dataset, const Config &config) {
    if (!index_ || !index_->is_trained) {
        KNOWHERE_THROW_MSG("index not initialize or trained");
    }

    std::lock_guard<std::mutex> lk(mutex_);
    GETTENSOR(dataset)

    index_->add(rows, (float *) p_data);
}

BinarySet IVF::Serialize() {
    if (!index_ || !index_->is_trained) {
        KNOWHERE_THROW_MSG("index not initialize or trained");
    }

    std::lock_guard<std::mutex> lk(mutex_);
    Seal();
    return SerializeImpl();
}

void IVF::Load(const BinarySet &index_binary) {
    std::lock_guard<std::mutex> lk(mutex_);
    LoadImpl(index_binary);
}

DatasetPtr IVF::Search(const DatasetPtr &dataset, const Config &config) {
    if (!index_ || !index_->is_trained) {
        KNOWHERE_THROW_MSG("index not initialize or trained");
    }

    auto k = config["k"].as<size_t>();

    GETTENSOR(dataset)

    // TODO(linxj): handle malloc exception
    auto elems = rows * k;
    auto res_ids = (int64_t *) malloc(sizeof(int64_t) * elems);
    auto res_dis = (float *) malloc(sizeof(float) * elems);

    search_impl(rows, (float*) p_data, k, res_dis, res_ids, config);
    //faiss::ivflib::search_with_parameters(index_.get(),
    //                                      rows,
    //                                      (float *) p_data,
    //                                      k,
    //                                      res_dis,
    //                                      res_ids,
    //                                      params.get());

    auto id_buf = MakeMutableBufferSmart((uint8_t *) res_ids, sizeof(int64_t) * elems);
    auto dist_buf = MakeMutableBufferSmart((uint8_t *) res_dis, sizeof(float) * elems);

    // TODO: magic
    std::vector<BufferPtr> id_bufs{nullptr, id_buf};
    std::vector<BufferPtr> dist_bufs{nullptr, dist_buf};

    auto int64_type = std::make_shared<arrow::Int64Type>();
    auto float_type = std::make_shared<arrow::FloatType>();

    auto id_array_data = arrow::ArrayData::Make(int64_type, elems, id_bufs);
    auto dist_array_data = arrow::ArrayData::Make(float_type, elems, dist_bufs);

    auto ids = std::make_shared<NumericArray<arrow::Int64Type>>(id_array_data);
    auto dists = std::make_shared<NumericArray<arrow::FloatType>>(dist_array_data);
    std::vector<ArrayPtr> array{ids, dists};

    return std::make_shared<Dataset>(array, nullptr);
}

void IVF::set_index_model(IndexModelPtr model) {
    std::lock_guard<std::mutex> lk(mutex_);

    auto rel_model = std::static_pointer_cast<IVFIndexModel>(model);

    // Deep copy here.
    index_.reset(faiss::clone_index(rel_model->index_.get()));
}

std::shared_ptr<faiss::IVFSearchParameters> IVF::GenParams(const Config &config) {
    auto params = std::make_shared<faiss::IVFPQSearchParameters>();
    params->nprobe = config.get_with_default("nprobe", size_t(1));
    //params->max_codes = config.get_with_default("max_codes", size_t(0));

    return params;
}

int64_t IVF::Count() {
    return index_->ntotal;
}

int64_t IVF::Dimension() {
    return index_->d;
}

void IVF::GenGraph(const int64_t &k, Graph &graph, const DatasetPtr &dataset, const Config &config) {
    GETTENSOR(dataset)

    auto ntotal = Count();

    auto batch_size = 100;
    auto tail_batch_size = ntotal % batch_size;
    auto batch_search_count = ntotal / batch_size;
    auto total_search_count = tail_batch_size == 0 ? batch_search_count : batch_search_count + 1;

    std::vector<float> res_dis(k * batch_size);
    graph.resize(ntotal);
    Graph res_vec(total_search_count);
    for (int i = 0; i < total_search_count; ++i) {
        auto b_size = i == total_search_count - 1 && tail_batch_size != 0 ? tail_batch_size : batch_size;

        auto &res = res_vec[i];
        res.resize(k * b_size);

        auto xq = p_data + batch_size * dim * i;
        search_impl(b_size, (float*)xq, k, res_dis.data(), res.data(), config);

        int tmp = 0;
        for (int j = 0; j < b_size; ++j) {
            auto &node = graph[batch_size * i + j];
            node.resize(k);
            for (int m = 0; m < k && tmp < k * b_size; ++m, ++tmp) {
                // TODO(linxj): avoid memcopy here.
                node[m] = res[tmp];
            }
        }
    }
}

void IVF::search_impl(int64_t n,
                      const float *data,
                      int64_t k,
                      float *distances,
                      int64_t *labels,
                      const Config &cfg) {
    auto params = GenParams(cfg);
    faiss::ivflib::search_with_parameters(index_.get(), n, (float *) data, k, distances, labels, params.get());
}

VectorIndexPtr IVF::CopyCpuToGpu(const int64_t& device_id, const Config &config) {
    if (auto res = FaissGpuResourceMgr::GetInstance().GetRes(device_id)){
        ResScope rs(res, device_id, false);
        auto gpu_index = faiss::gpu::index_cpu_to_gpu(res->faiss_res.get(), device_id, index_.get());

        std::shared_ptr<faiss::Index> device_index;
        device_index.reset(gpu_index);
        return std::make_shared<GPUIVF>(device_index, device_id, res);
    } else {
        KNOWHERE_THROW_MSG("CopyCpuToGpu Error, can't get gpu_resource");
    }
}

VectorIndexPtr IVF::Clone() {
    std::lock_guard<std::mutex> lk(mutex_);

    auto clone_index = faiss::clone_index(index_.get());
    std::shared_ptr<faiss::Index> new_index;
    new_index.reset(clone_index);
    return Clone_impl(new_index);
}

VectorIndexPtr IVF::Clone_impl(const std::shared_ptr<faiss::Index> &index) {
    return std::make_shared<IVF>(index);
}

void IVF::Seal() {
    if (!index_ || !index_->is_trained) {
        KNOWHERE_THROW_MSG("index not initialize or trained");
    }
    SealImpl();
}


IVFIndexModel::IVFIndexModel(std::shared_ptr<faiss::Index> index) : FaissBaseIndex(std::move(index)) {}

BinarySet IVFIndexModel::Serialize() {
    if (!index_ || !index_->is_trained) {
        KNOWHERE_THROW_MSG("indexmodel not initialize or trained");
    }
    std::lock_guard<std::mutex> lk(mutex_);
    return SerializeImpl();
}

void IVFIndexModel::Load(const BinarySet &binary_set) {
    std::lock_guard<std::mutex> lk(mutex_);
    LoadImpl(binary_set);
}

void IVFIndexModel::SealImpl() {
    // do nothing
}





}
}
