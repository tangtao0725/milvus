// Copyright (C) 2019-2020 Zilliz. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except in compliance
// with the License. You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the License
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied. See the License for the specific language governing permissions and limitations under the License.

#include "server/delivery/request/CreateCollectionReq.h"
#include "db/Utils.h"
#include "server/DBWrapper.h"
#include "server/ValidationUtil.h"
#include "server/delivery/request/BaseReq.h"
#include "server/web_impl/Constants.h"
#include "utils/Log.h"
#include "utils/TimeRecorder.h"

#include <fiu-local.h>
#include <src/db/snapshot/Context.h>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace milvus {
namespace server {

CreateCollectionReq::CreateCollectionReq(const std::shared_ptr<milvus::server::Context>& context,
                                         const std::string& collection_name,
                                         std::unordered_map<std::string, FieldSchema>& fields,
                                         milvus::json& extra_params)
    : BaseReq(context, BaseReq::kCreateCollection),
      collection_name_(collection_name),
      fields_(fields),
      extra_params_(extra_params) {
}

BaseReqPtr
CreateCollectionReq::Create(const std::shared_ptr<milvus::server::Context>& context, const std::string& collection_name,
                            std::unordered_map<std::string, FieldSchema>& fields, milvus::json& extra_params) {
    return std::shared_ptr<BaseReq>(new CreateCollectionReq(context, collection_name, fields, extra_params));
}

Status
CreateCollectionReq::OnExecute() {
    std::string hdr = "CreateCollectionReq(collection=" + collection_name_ + ")";
    TimeRecorderAuto rc(hdr);

    try {
        // step 1: check arguments
        auto status = ValidateCollectionName(collection_name_);
        fiu_do_on("CreateCollectionReq.OnExecute.invalid_collection_name",
                  status = Status(milvus::SERVER_UNEXPECTED_ERROR, ""));
        if (!status.ok()) {
            return status;
        }

        rc.RecordSection("check validation");

        // step 2: create snapshot collection context
        engine::snapshot::CreateCollectionContext create_collection_context;
        auto collection_schema = std::make_shared<engine::snapshot::Collection>(collection_name_, extra_params_);
        create_collection_context.collection = collection_schema;
        for (auto& field_kv : fields_) {
            auto& field_name = field_kv.first;
            auto& field_schema = field_kv.second;

            auto& field_type = field_schema.field_type_;
            auto& field_params = field_schema.field_params_;
            auto& index_params = field_schema.index_params_;

            std::cout << index_params.dump() << std::endl;
            std::string index_name;
            if (index_params.contains("name")) {
                index_name = index_params["name"];
            }

            std::cout << field_params.dump() << std::endl;
            if (field_type == engine::FieldType::VECTOR_FLOAT || field_type == engine::FieldType::VECTOR_BINARY) {
                if (!field_params.contains(engine::PARAM_DIMENSION)) {
                    return Status(SERVER_INVALID_VECTOR_DIMENSION, "Dimension not defined in field_params");
                }
            }

            auto field = std::make_shared<engine::snapshot::Field>(field_name, 0, field_type, field_params);
            auto field_element = std::make_shared<engine::snapshot::FieldElement>(
                0, 0, index_name, engine::FieldElementType::FET_INDEX, index_params);
            create_collection_context.fields_schema[field] = {field_element};
        }

        if (!extra_params_.contains(engine::PARAM_SEGMENT_SIZE)) {
            return Status(SERVER_UNEXPECTED_ERROR, "Segment size not defined");
        } else {
            auto segment_size = extra_params_[engine::PARAM_SEGMENT_SIZE].get<int64_t>();
            STATUS_CHECK(ValidateCollectionIndexFileSize(segment_size));
        }

        // step 3: create collection
        status = DBWrapper::DB()->CreateCollection(create_collection_context);
        fiu_do_on("CreateCollectionReq.OnExecute.invalid_db_execute",
                  status = Status(milvus::SERVER_UNEXPECTED_ERROR, ""));
        if (!status.ok()) {
            // collection could exist
            if (status.code() == DB_ALREADY_EXIST) {
                return Status(SERVER_INVALID_COLLECTION_NAME, status.message());
            }
            return status;
        }

        rc.ElapseFromBegin("done");
    } catch (std::exception& ex) {
        return Status(SERVER_UNEXPECTED_ERROR, ex.what());
    }

    return Status::OK();
}

}  // namespace server
}  // namespace milvus
