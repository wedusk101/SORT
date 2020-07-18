/*
    This file is a part of SORT(Simple Open Ray Tracing), an open-source cross
    platform physically based renderer.

    Copyright (c) 2011-2020 by Jiayin Cao - All rights reserved.

    SORT is a free software written for educational purpose. Anyone can distribute
    or modify it under the the terms of the GNU General Public License Version 3 as
    published by the Free Software Foundation. However, there is NO warranty that
    all components are functional in a perfect manner. Without even the implied
    warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
    General Public License for more details.

    You should have received a copy of the GNU General Public License along with
    this program. If not, see <http://www.gnu.org/licenses/gpl-3.0.html>.
 */

// This is a temporary quick solution to enable multi-thread texture loading
// It is by no means a very good idea to parallel a bunch of IO bound threads.
// However, my newly planned job system is far from being ready yet, I'll live with it for now.
// This async loading eventually will be less useful since I'm planning to implement a texture cache
// system in the future to do lazy texture loading in the future.
#define ASYNC_TEXTURE_LOADING

#ifdef ASYNC_TEXTURE_LOADING
#include <future>
#endif

#include "matmanager.h"
#include "material/material.h"
#include "stream/stream.h"
#include "core/profile.h"
#include "core/globalconfig.h"
#include "core/log.h"
#include "core/timer.h"
#include "scatteringevent/bsdf/merl.h"
#include "scatteringevent/bsdf/fourierbxdf.h"
#include "texture/imagetexture2d.h"

namespace {
    struct ShaderResourceBinding {
        std::string resource_handle_name;
        std::string shader_resource_name;
    };
}

void ShaderCompiling_Task::Execute(){
    TIMING_EVENT( "Compiling Shader" );
    m_material->BuildMaterial();
}

ShaderArgumentTypeEnum shader_arg_type_from_sid(const StringID id) {
    if (id == "closure"_sid)
        return ShaderArgumentTypeEnum::TSL_TYPE_CLOSURE;
    else if (id == "vector"_sid)
        return ShaderArgumentTypeEnum::TSL_TYPE_FLOAT3;
    else if (id == "float"_sid)
        return ShaderArgumentTypeEnum::TSL_TYPE_FLOAT;
    return ShaderArgumentTypeEnum::TSL_TYPE_INVALID;
}

#ifdef ASYNC_TEXTURE_LOADING
static bool async_load_resource(Resource* resource, std::string filename) {
    return resource->LoadResource(filename);
}
#endif

// parse material file and add the materials into the manager
unsigned MatManager::ParseMatFile( IStreamBase& stream ){
    SORT_PROFILE("Parsing Materials");

    const auto current_task = GetCurrentTask();
    const auto dependencies = current_task->GetDependencies();

    auto resource_cnt = 0u;
    stream >> resource_cnt;

#ifdef ASYNC_TEXTURE_LOADING
    std::vector<std::future<bool>>      async_resource_reading;
#endif

    for (auto i = 0u; i < resource_cnt; ++i) {
        std::string resource_file;
        StringID resource_type;
        stream >> resource_file >> resource_type;

        Resource* ptr_resource = nullptr;

        if (0 == m_resources.count(resource_file)) {
            if (resource_type == SID("MerlBRDFMeasuredData")) {
                m_resources[resource_file] = std::make_unique<MerlData>();
                ptr_resource = m_resources[resource_file].get();
            }
            else if (resource_type == SID("FourierBRDFMeasuredData")) {
                m_resources[resource_file] = std::make_unique<FourierBxdfData>();
                ptr_resource = m_resources[resource_file].get();
            }
            else if (resource_type == SID("Texture2D")) {
                m_resources[resource_file] = std::make_unique<ImageTexture2D>();
                ptr_resource = m_resources[resource_file].get();
            }

            if (!ptr_resource) {
                sAssertMsg(false, MATERIAL, "Resource type not supported!");
            }
            else {
#ifdef ASYNC_TEXTURE_LOADING
                async_resource_reading.push_back(std::async(std::launch::async, async_load_resource, ptr_resource, resource_file));
#else
                ptr_resource->LoadResource(resource_file);
#endif
            }
        }
    }

    const bool noMaterialSupport = g_noMaterial;

    StringID material_type;
    while (true) {
        stream >> material_type;

        if (material_type == SID("End of Material"))
            break;
        else if (material_type == SID("ShaderUnitTemplate")) {
            // shader type, maybe I should use string id here.
            std::string shader_node_type;
            stream >> shader_node_type;

            // stream the shader source code
            std::string source_code;
            stream >> source_code;

            /**< Shader reousrce binding. */
            std::vector<ShaderResourceBinding>  m_shader_resources_binding;

            unsigned int shader_resources = 0;
            stream >> shader_resources;
            for (auto i = 0u; i < shader_resources; ++i) {
                ShaderResourceBinding srb;
                stream >> srb.resource_handle_name >> srb.shader_resource_name;
                m_shader_resources_binding.push_back(srb);
            }

            // compile the shader unit template
            auto shading_context = GetShadingContext();

            // allocate the shader unit template
            const auto shader_unit_template = shading_context->begin_shader_unit_template(shader_node_type);
            
            // register tsl global
            TslGlobal::shader_unit_register(shader_unit_template.get());

            // bind shader resources
            for (auto sr : m_shader_resources_binding) {
                auto resource = MatManager::GetSingleton().GetResource(sr.shader_resource_name);
                shader_unit_template->register_shader_resource(sr.resource_handle_name, (const ShaderResourceHandle*)resource);
            }

            // compile the shader unit
            const auto ret = shading_context->compile_shader_unit_template(shader_unit_template.get(), source_code.c_str());

            // indicate the end of shader unit compilation
            shading_context->end_shader_unit_template(shader_unit_template.get());

            // push it if it compiles the shader successful
            if( ret )
                m_shader_units[shader_node_type] = shader_unit_template;
        }
        else if (material_type == SID("ShaderGroupTemplate")) {
            // The following logic is very similar with 
            std::string shader_template_type;
            stream >> shader_template_type;

            unsigned shader_unit_cnt = 0;
            stream >> shader_unit_cnt;

            TSL_ShaderData shader_data;

            for (auto i = 0u; i < shader_unit_cnt; ++i) {
                // parse surface shader
                ShaderSource shader_source;
                stream >> shader_source.name >> shader_source.type;

                auto parameter_cnt = 0u;
                stream >> parameter_cnt;
                for (auto j = 0u; j < parameter_cnt; ++j) {
                    ShaderParamDefaultValue default_value;
                    default_value.shader_unit_name = shader_source.name;
                    stream >> default_value.shader_unit_param_name;
                    int channel_num = 0;
                    stream >> channel_num;
                    // currently only float and float3 are supported for now
                    if (channel_num == 1) {
                        float x;
                        stream >> x;
                        default_value.default_value = x;
                    }
                    else if (channel_num == 3) {
                        float x, y, z;
                        stream >> x >> y >> z;
                        default_value.default_value = Tsl_Namespace::make_float3(x, y, z);
                    }
                    else if (channel_num == 4) { // this is fairly ugly, but it works, I will find time to refactor it later.
                        std::string str;
                        stream >> str;
                        default_value.default_value = make_tsl_global_ref(str);
                    }

                    m_paramDefaultValues.push_back(default_value);
                }

                shader_data.m_sources.push_back(shader_source);
            }

            auto connection_cnt = 0u;
            stream >> connection_cnt;
            for (auto i = 0u; i < connection_cnt; ++i) {
                ShaderConnection connection;
                stream >> connection.source_shader >> connection.source_property;
                stream >> connection.target_shader >> connection.target_property;
                shader_data.m_connections.push_back(connection);
            }

            // compiling the shader group template
            std::unordered_map<std::string, std::shared_ptr<Tsl_Namespace::ShaderUnitTemplate>> shader_units;
            for (const auto& shader : shader_data.m_sources)
                shader_units[shader.name] = MatManager::GetSingleton().GetShaderUnitTemplate(shader.type);

            // begin compiling shader group
            auto shader_group = BeginShaderGroup(shader_template_type);
            if (!shader_group)
                continue;

            // register tsl global
            TslGlobal::shader_unit_register(shader_group.get());

            // expose arguments in output node
            std::string root_shader_name;
            stream >> root_shader_name;
            unsigned int exposed_out_arg_cnt = 0;
            stream >> exposed_out_arg_cnt;
            for (auto i = 0u; i < exposed_out_arg_cnt; ++i) {
                std::string arg_name;
                stream >> arg_name;

                StringID data_type;
                stream >> data_type;

                // expose the shader interface
                ExposedArgDescriptor arg;
                arg.m_name = arg_name;
                arg.m_type = shader_arg_type_from_sid(data_type);
                arg.m_is_output = true;
                shader_group->expose_shader_argument(root_shader_name, arg_name, arg);
            }

            std::string shader_group_input_name;
            stream >> shader_group_input_name;

            if (!shader_group_input_name.empty()) {
                unsigned int exposed_in_arg_cnt = 0;
                stream >> exposed_in_arg_cnt;
                for (auto i = 0u; i < exposed_in_arg_cnt; ++i) {
                    std::string arg_name;
                    stream >> arg_name;

                    StringID data_type;
                    stream >> data_type;

                    // expose the shader interface
                    ExposedArgDescriptor arg;
                    arg.m_name = arg_name;
                    arg.m_type = shader_arg_type_from_sid(data_type);
                    arg.m_is_output = false;
                    shader_group->expose_shader_argument(shader_group_input_name, arg_name, arg);
                }
            }

            for (auto su : shader_units) {
                const auto is_root = (su.first == root_shader_name);
                const auto ret = shader_group->add_shader_unit(su.first, su.second, is_root);
                if (!ret)
                    continue;
            }

            // connect the shader units
            for (auto connection : shader_data.m_connections)
                shader_group->connect_shader_units(connection.source_shader, connection.source_property, connection.target_shader, connection.target_property);

            // update default values
            for (const auto& dv : m_paramDefaultValues)
                shader_group->init_shader_input(dv.shader_unit_name, dv.shader_unit_param_name, dv.default_value);

            // end building the shader group
            auto ret = EndShaderGroup(shader_group.get());

            // push it if it compiles the shader successful
            if (TSL_Resolving_Status::TSL_Resolving_Succeed == ret)
                m_shader_units[shader_template_type] = shader_group;
        }
        else if (material_type == SID("Material")) {
            // allocate a new material
            auto mat = std::make_unique<Material>();

            // serialize the material
            mat->Serialize(stream);

            if (LIKELY(!noMaterialSupport)) {
                // build the material
                mat->BuildMaterial();

                // push the compiled material in the pool
                m_matPool.push_back(std::move(mat));
            }
        }
        else {
            sAssertMsg(false, MATERIAL, "Serialization is broken.");
        }
    }

#ifdef ASYNC_TEXTURE_LOADING
    std::for_each(async_resource_reading.begin(), async_resource_reading.end(), [](std::future<bool>& promise) { promise.wait(); });
#endif

    return (unsigned int)m_matPool.size();
}

const Resource* MatManager::GetResource(const std::string& name) const {
    auto it = m_resources.find(name);
    if (it == m_resources.end())
        return nullptr;
    return it->second.get();
}

const MaterialBase* MatManager::CreateMaterialProxy(const MaterialBase& material) {
    m_matPool.push_back(std::move(std::make_unique<MaterialProxy>(material)));
    return m_matPool.back().get();
}

std::shared_ptr<ShaderUnitTemplate> MatManager::GetShaderUnitTemplate(const std::string& name_id) const {
    auto it = m_shader_units.find(name_id);
    if (it == m_shader_units.end())
        return nullptr;
    return it->second;
}