/*
Copyright(c) 2016 Christopher Joseph Dean Schaefer

This software is provided 'as-is', without any express or implied
warranty.In no event will the authors be held liable for any damages
arising from the use of this software.

Permission is granted to anyone to use this software for any purpose,
including commercial applications, and to alter it and redistribute it
freely, subject to the following restrictions :

1. The origin of this software must not be misrepresented; you must not
claim that you wrote the original software.If you use this software
in a product, an acknowledgment in the product documentation would be
appreciated but is not required.
2. Altered source versions must be plainly marked as such, and must not be
misrepresented as being the original software.
3. This notice may not be removed or altered from any source distribution.
*/

#include <vulkan/vk_sdk_platform.h>
#include "d3d9.h"
#include "D3D11Shader.h"
#include "ShaderConverter.h"
#include <boost/log/trivial.hpp>
#include <fstream>

/*
http://timjones.io/blog/archive/2015/09/02/parsing-direct3d-shader-bytecode
https://msdn.microsoft.com/en-us/library/bb219840(VS.85).aspx#Shader_Binary_Format
http://stackoverflow.com/questions/2545704/format-of-compiled-directx9-shader-files
https://msdn.microsoft.com/en-us/library/windows/hardware/ff552891(v=vs.85).aspx
https://github.com/ValveSoftware/ToGL
https://www.khronos.org/registry/spir-v/specs/1.2/SPIRV.html
*/

const uint32_t mEndToken = 0x0000FFFF;
const uint16_t mShaderTypePixel = 0xFFFF;
const uint16_t mShaderTypeVertex = 0xFFFE;

#define PACK(c0, c1, c2, c3) \
    (((uint32_t)(uint8_t)(c0) << 24) | \
    ((uint32_t)(uint8_t)(c1) << 16) | \
    ((uint32_t)(uint8_t)(c2) << 8) | \
    ((uint32_t)(uint8_t)(c3)))

/*
Generatorís magic number. It is associated with the tool that generated
the module. Its value does not affect any semantics, and is allowed to be 0.
Using a non-0 value is encouraged, and can be registered with
Khronos at https://www.khronos.org/registry/spir-v/api/spir-v.xml.
*/
#define SPIR_V_GENERATORS_NUMBER 0x00000000

ShaderConverter::ShaderConverter(VkDevice device)
	: mDevice(device)
{

}

Token ShaderConverter::GetNextToken()
{
	Token token;
	token.i = *(mNextToken++);
	return token;
}

void ShaderConverter::SkipTokens(uint32_t numberToSkip)
{
	mNextToken += numberToSkip;
}

uint32_t ShaderConverter::GetNextId()
{
	return mNextId++;
}

void ShaderConverter::SkipIds(uint32_t numberToSkip)
{
	mNextId += numberToSkip;
}

uint32_t ShaderConverter::GetOpcode(uint32_t token)
{
	return (token & D3DSI_OPCODE_MASK);
}

uint32_t ShaderConverter::GetOpcodeData(uint32_t token)
{
	return ((token & D3DSP_OPCODESPECIFICCONTROL_MASK) >> D3DSP_OPCODESPECIFICCONTROL_SHIFT);
}

uint32_t ShaderConverter::GetTextureType(uint32_t token)
{
	return (token & D3DSP_TEXTURETYPE_MASK); // Note this one doesn't shift due to weird D3DSAMPLER_TEXTURE_TYPE enum
}

_D3DSHADER_PARAM_REGISTER_TYPE ShaderConverter::GetRegisterType(uint32_t token)
{
	return (_D3DSHADER_PARAM_REGISTER_TYPE)(((token & D3DSP_REGTYPE_MASK2) >> D3DSP_REGTYPE_SHIFT2) | ((token & D3DSP_REGTYPE_MASK) >> D3DSP_REGTYPE_SHIFT));
}

uint32_t ShaderConverter::GetRegisterNumber(uint32_t token)
{
	return token & D3DSP_REGNUM_MASK;
}

uint32_t ShaderConverter::GetUsage(uint32_t token)
{
	return token & D3DSP_DCL_USAGE_MASK;
}

uint32_t ShaderConverter::GetUsageIndex(uint32_t token)
{
	return (token & D3DSP_DCL_USAGEINDEX_MASK) >> D3DSP_DCL_USAGEINDEX_SHIFT;
}

uint32_t ShaderConverter::GetSpirVTypeId(spv::Op registerType)
{
	TypeDescription description;

	description.PrimaryType = registerType;

	return GetSpirVTypeId(description);
}

uint32_t ShaderConverter::GetSpirVTypeId(spv::Op registerType1, spv::Op registerType2)
{
	TypeDescription description;

	description.PrimaryType = registerType1;
	description.SecondaryType = registerType2;

	return GetSpirVTypeId(description);
}

uint32_t ShaderConverter::GetSpirVTypeId(TypeDescription& registerType)
{
	boost::container::flat_map<TypeDescription, uint32_t>::iterator it = mTypeIdPairs.find(registerType);

	if (it == mTypeIdPairs.end())
	{
		uint32_t id = GetNextId();
		mTypeIdPairs[registerType] = id;
		mIdTypePairs[id] = registerType;

		switch (registerType.PrimaryType)
		{
		case spv::OpTypeBool:
			mTypeInstructions.push_back(2); //size
			mTypeInstructions.push_back(registerType.PrimaryType); //Type
			mTypeInstructions.push_back(id); //Id
			break;
		case spv::OpTypeInt:
			mTypeInstructions.push_back(4); //size
			mTypeInstructions.push_back(registerType.PrimaryType); //Type
			mTypeInstructions.push_back(id); //Id
			mTypeInstructions.push_back(32); //Number of bits.
			mTypeInstructions.push_back(0); //Signedness (0 = unsigned,1 = signed)
			break;
		case spv::OpTypeFloat:
			mTypeInstructions.push_back(3); //size
			mTypeInstructions.push_back(registerType.PrimaryType); //Type
			mTypeInstructions.push_back(id); //Id
			mTypeInstructions.push_back(32); //Number of bits.
			break;
		case spv::OpTypeVector:
			//Matrix and Vector type opcodes are laid out the same but exchange component for column.
		case spv::OpTypeMatrix:
			mTypeInstructions.push_back(4); //size
			mTypeInstructions.push_back(registerType.PrimaryType); //Type
			mTypeInstructions.push_back(id); //Id
			mTypeInstructions.push_back(GetSpirVTypeId(registerType.SecondaryType)); //Component/Column Type
			mTypeInstructions.push_back(registerType.ComponentCount);
			break;
		case spv::OpTypePointer:
			mTypeInstructions.push_back(4); //size
			mTypeInstructions.push_back(registerType.PrimaryType); //Type
			mTypeInstructions.push_back(id); //Id
			mTypeInstructions.push_back(spv::StorageClassInput); //Storage Class
			mTypeInstructions.push_back(GetSpirVTypeId(registerType.SecondaryType, registerType.TernaryType)); // Type
			break;
		case spv::OpTypeSampler:
			mTypeInstructions.push_back(2); //size
			mTypeInstructions.push_back(registerType.PrimaryType); //Type
			mTypeInstructions.push_back(id); //Id
			break;
		case spv::OpTypeFunction:
			mTypeInstructions.push_back(3 + registerType.Arguments.size()); //size
			mTypeInstructions.push_back(registerType.PrimaryType); //Type
			mTypeInstructions.push_back(id); //Id
			mTypeInstructions.push_back(GetSpirVTypeId(registerType.SecondaryType)); //Return Type
			
			//Right now there is no comparison on arguments so we are assuming that functions with the same return type are the same. This will need to be expanded later when we're using functions other than the default entry point.
			for (size_t i = 0; i < registerType.Arguments.size(); i++)
			{
				mTypeInstructions.push_back(registerType.Arguments[i]); //Argument Id
			}
			break;
		default:
			BOOST_LOG_TRIVIAL(warning) << "GetSpirVTypeId - Unsupported data type " << registerType.PrimaryType;
			break;
		}

		return id;
	}
	else
	{
		return it->second;
	}

	/*
		case spv::OpTypeImage:
			break;
		case spv::OpTypeSampledImage:
			break;
		case spv::OpTypeArray:
			break;
		case spv::OpTypeRuntimeArray:
			break;
		case spv::OpTypeStruct:
			break;
		case spv::OpTypeOpaque:
			break;
		case spv::OpTypeEvent:
			break;
		case spv::OpTypeDeviceEvent:
			break;
		case spv::OpTypeReserveId:
			break;
		case spv::OpTypeQueue:
			break;
		case spv::OpTypePipe:
			break;
		case spv::OpTypeForwardPointer:
			break;
	*/
}

/*
SPIR-V is SSA so this method will generate a new id with the type of the old one when a new "register" is needed.
To handle this result registers will get a new Id each type. The result Id can be used as an input to other operations so this will work fine.
To make sure each call gets the latest id number the lookups must be updated.
*/
uint32_t ShaderConverter::GetNextVersionId(uint32_t registerNumber)
{
	uint32_t id = GetNextId();
	//uint32_t oldId = mRegisterIdPairs[registerId];

	mRegisterIdPairs[registerNumber] = id;
	mIdRegisterPairs[id] = registerNumber;

	return id;
}

TypeDescription ShaderConverter::GetTypeByRegister(uint32_t registerNumber)
{
	TypeDescription dataType;

	boost::container::flat_map<uint32_t, uint32_t>::iterator it1 = mRegisterIdPairs.find(registerNumber);
	if (it1 != mRegisterIdPairs.end())
	{
		boost::container::flat_map<uint32_t, TypeDescription>::iterator it2 = mIdTypePairs.find(it1->second);
		if (it2 != mIdTypePairs.end())
		{
			dataType = it2->second;
		}
		else
		{
			dataType.PrimaryType = spv::OpTypeFloat;
		}
	}
	else
	{
		dataType.PrimaryType = spv::OpTypeFloat;
	}

	return dataType;
}

void ShaderConverter::CombineSpirVOpCodes()
{
	mInstructions.insert(std::end(mInstructions), std::begin(mCapabilityInstructions), std::end(mCapabilityInstructions));
	mInstructions.insert(std::end(mInstructions), std::begin(mExtensionInstructions), std::end(mExtensionInstructions));
	mInstructions.insert(std::end(mInstructions), std::begin(mImportExtendedInstructions), std::end(mImportExtendedInstructions));
	mInstructions.insert(std::end(mInstructions), std::begin(mMemoryModelInstructions), std::end(mMemoryModelInstructions));
	mInstructions.insert(std::end(mInstructions), std::begin(mEntryPointInstructions), std::end(mEntryPointInstructions));
	mInstructions.insert(std::end(mInstructions), std::begin(mExecutionModeInstructions), std::end(mExecutionModeInstructions));

	mInstructions.insert(std::end(mInstructions), std::begin(mStringInstructions), std::end(mStringInstructions));
	mInstructions.insert(std::end(mInstructions), std::begin(mSourceExtensionInstructions), std::end(mSourceExtensionInstructions));
	mInstructions.insert(std::end(mInstructions), std::begin(mSourceInstructions), std::end(mSourceInstructions));
	mInstructions.insert(std::end(mInstructions), std::begin(mSourceContinuedInstructions), std::end(mSourceContinuedInstructions));
	mInstructions.insert(std::end(mInstructions), std::begin(mNameInstructions), std::end(mNameInstructions));
	mInstructions.insert(std::end(mInstructions), std::begin(mMemberNameInstructions), std::end(mMemberNameInstructions));

	mInstructions.insert(std::end(mInstructions), std::begin(mDecorateInstructions), std::end(mDecorateInstructions));
	mInstructions.insert(std::end(mInstructions), std::begin(mMemberDecorateInstructions), std::end(mMemberDecorateInstructions));
	mInstructions.insert(std::end(mInstructions), std::begin(mGroupDecorateInstructions), std::end(mGroupDecorateInstructions));
	mInstructions.insert(std::end(mInstructions), std::begin(mGroupMemberDecorateInstructions), std::end(mGroupMemberDecorateInstructions));
	mInstructions.insert(std::end(mInstructions), std::begin(mDecorationGroupInstructions), std::end(mDecorationGroupInstructions));

	mInstructions.insert(std::end(mInstructions), std::begin(mTypeInstructions), std::end(mTypeInstructions));
	mInstructions.insert(std::end(mInstructions), std::begin(mFunctionDeclarationInstructions), std::end(mFunctionDeclarationInstructions));
	mInstructions.insert(std::end(mInstructions), std::begin(mFunctionDefinitionInstructions), std::end(mFunctionDefinitionInstructions));

	mCapabilityInstructions.clear();
	mExtensionInstructions.clear();
	mImportExtendedInstructions.clear();
	mMemoryModelInstructions.clear();
	mEntryPointInstructions.clear();
	mExecutionModeInstructions.clear();

	mStringInstructions.clear();
	mSourceExtensionInstructions.clear();
	mSourceInstructions.clear();
	mSourceContinuedInstructions.clear();
	mNameInstructions.clear();
	mMemberNameInstructions.clear();

	mDecorateInstructions.clear();
	mMemberDecorateInstructions.clear();
	mGroupDecorateInstructions.clear();
	mGroupMemberDecorateInstructions.clear();
	mDecorationGroupInstructions.clear();

	mTypeInstructions.clear();
	mFunctionDeclarationInstructions.clear();
	mFunctionDefinitionInstructions.clear();
}

void ShaderConverter::CreateSpirVModule()
{
	VkResult result = VK_SUCCESS;
	VkShaderModuleCreateInfo moduleCreateInfo = {};
	moduleCreateInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	moduleCreateInfo.pNext = NULL;
	moduleCreateInfo.codeSize = mInstructions.size() * sizeof(uint32_t);
	moduleCreateInfo.pCode = (uint32_t*)mInstructions.data(); //Why is this uint32_t* if the size is in bytes?
	moduleCreateInfo.flags = 0;
	result = vkCreateShaderModule(mDevice, &moduleCreateInfo, NULL, &mConvertedShader.ShaderModule);

	if (result != VK_SUCCESS)
	{
		BOOST_LOG_TRIVIAL(fatal) << "ShaderConverter::CreateSpirVModule vkCreateShaderModule failed with return code of " << result;
		return;
	}

#ifdef _DEBUG
	//Start Debug Code
	if (!mIsVertexShader)
	{
		std::ofstream outFile("fragment.spv", std::ios::out | std::ios::binary);
		outFile.write((char*)mInstructions.data(), mInstructions.size() * sizeof(uint32_t));
	}
	else
	{
		std::ofstream outFile("vertex.spv", std::ios::out | std::ios::binary);
		outFile.write((char*)mInstructions.data(), mInstructions.size() * sizeof(uint32_t));
	}
	//End Debug Code
#endif

	mInstructions.clear();
}

//https://msdn.microsoft.com/en-us/library/windows/hardware/ff552693(v=vs.85).aspx
void ShaderConverter::Process_DCL()
{
	Token token = GetNextToken();
	Token registerToken = GetNextToken();
	uint32_t usage = GetUsage(token.i);
	uint32_t usageIndex = GetUsageIndex(token.i);
	uint32_t registerNumber = GetRegisterNumber(registerToken.i);
	_D3DSHADER_PARAM_REGISTER_TYPE registerType = GetRegisterType(registerToken.i);
	uint32_t tokenId = GetNextVersionId(registerNumber);
	TypeDescription typeDescription;
	typeDescription.PrimaryType = spv::OpTypePointer;
	typeDescription.SecondaryType = spv::OpTypeVector;
	typeDescription.TernaryType = spv::OpTypeFloat;
	uint32_t resultTypeId = GetSpirVTypeId(typeDescription);
	uint32_t registerComponents = (registerToken.i & D3DSP_WRITEMASK_ALL) >> 16;

	mInterfaceIds.push_back(tokenId); //Used by entry point opcode.

	//Magic numbers from ToGL (no whole numbers?)
	switch (registerComponents)
	{
	case 1: //float
		typeDescription.SecondaryType = spv::OpTypeFloat;
		typeDescription.TernaryType = spv::OpTypeVoid;
		break;
	case 3: //vec2
		typeDescription.ComponentCount = 2;
		break;
	case 7: //vec3
		typeDescription.ComponentCount = 3;
		break;
	case 0xF: //vec4
		typeDescription.ComponentCount = 4;
		break;
	default:
		BOOST_LOG_TRIVIAL(warning) << "Process_DCL - Unsupported component type " << registerComponents;
		break;
	}

	mIdTypePairs[tokenId] = typeDescription;

	if (mIsVertexShader)
	{
		if (mMajorVersion >= 3 && registerType == D3DSPR_OUTPUT)
		{
			mTypeInstructions.push_back(4); //size
			mTypeInstructions.push_back(spv::OpVariable); //opcode
			mTypeInstructions.push_back(GetSpirVTypeId(typeDescription)); //ResultType (Id) Must be OpTypePointer with the pointer's type being what you care about.
			mTypeInstructions.push_back(tokenId); //Result (Id)
			mTypeInstructions.push_back(spv::StorageClassOutput); //Storage Class
			//Optional initializer

			if (usage == D3DDECLUSAGE_POSITION)
			{
				mPositionRegister = usageIndex; //might need this later.
			}

		}
		else if (registerType == D3DSPR_SAMPLER)
		{
			BOOST_LOG_TRIVIAL(warning) << "Process_DCL - Unsupported declare type D3DSPR_SAMPLER";
		}
		else if (registerType == D3DSPR_INPUT)
		{
			mTypeInstructions.push_back(4); //size
			mTypeInstructions.push_back(spv::OpVariable); //opcode
			mTypeInstructions.push_back(GetSpirVTypeId(typeDescription)); //ResultType (Id) Must be OpTypePointer with the pointer's type being what you care about.
			mTypeInstructions.push_back(tokenId); //Result (Id)
			mTypeInstructions.push_back(spv::StorageClassInput); //Storage Class
			//Optional initializer

			mConvertedShader.mVertexInputAttributeDescription[mConvertedShader.mVertexInputAttributeDescriptionCount].binding = 0;//element.Stream; TODO:REVISIT
			mConvertedShader.mVertexInputAttributeDescription[mConvertedShader.mVertexInputAttributeDescriptionCount].location = mConvertedShader.mVertexInputAttributeDescriptionCount;
			mConvertedShader.mVertexInputAttributeDescription[mConvertedShader.mVertexInputAttributeDescriptionCount].offset = 0;//element.Offset;

			switch (typeDescription.ComponentCount)
			{
			case 1: // 1D float expanded to (value, 0., 0., 1.)
				mConvertedShader.mVertexInputAttributeDescription[mConvertedShader.mVertexInputAttributeDescriptionCount].format = VK_FORMAT_R32_SFLOAT;
				break;
			case 2:  // 2D float expanded to (value, value, 0., 1.)
				mConvertedShader.mVertexInputAttributeDescription[mConvertedShader.mVertexInputAttributeDescriptionCount].format = VK_FORMAT_R32G32_SFLOAT;
				break;
			case 3: // 3D float expanded to (value, value, value, 1.)
				mConvertedShader.mVertexInputAttributeDescription[mConvertedShader.mVertexInputAttributeDescriptionCount].format = VK_FORMAT_R32G32B32_SFLOAT;
				break;
			case 4:  // 4D float
				mConvertedShader.mVertexInputAttributeDescription[mConvertedShader.mVertexInputAttributeDescriptionCount].format = VK_FORMAT_R32G32B32A32_SFLOAT;
			default:
				break;
			}

			mConvertedShader.mVertexInputAttributeDescriptionCount++;
		}
		else
		{
			BOOST_LOG_TRIVIAL(warning) << "Process_DCL - Unsupported declare type " << registerType;
		}
	}
	else //This is a pixel shader (at least for d3d9)
	{
		if (mMajorVersion >= 3 && registerType == D3DSPR_OUTPUT)
		{
			mTypeInstructions.push_back(4); //size
			mTypeInstructions.push_back(spv::OpVariable); //opcode
			mTypeInstructions.push_back(GetSpirVTypeId(typeDescription)); //ResultType (Id) Must be OpTypePointer with the pointer's type being what you care about.
			mTypeInstructions.push_back(tokenId); //Result (Id)
			mTypeInstructions.push_back(spv::StorageClassOutput); //Storage Class
			//Optional initializer
		}
		else if (registerType == D3DSPR_SAMPLER)
		{
			uint32_t textureType = GetTextureType(token.i);

			//switch (textureType)
			//{
			//case D3DSTT_2D:
			//	break;
			//case D3DSTT_CUBE:
			//	break;
			//case D3DSTT_VOLUME:
			//	break;
			//case D3DSTT_UNKNOWN:
			//	break;
			//default:
			//	break;
			//}	

			mTypeInstructions.push_back(4); //size
			mTypeInstructions.push_back(spv::OpVariable); //opcode
			mTypeInstructions.push_back(GetSpirVTypeId(spv::OpTypePointer, spv::OpTypeSampler)); //ResultType (Id) Must be OpTypePointer with the pointer's type being what you care about.
			mTypeInstructions.push_back(tokenId); //Result (Id)
			mTypeInstructions.push_back(spv::StorageClassUniform); //Storage Class  (need to make sure this shouldn't be StorageClassImage)
			//Optional initializer

			mConvertedShader.mDescriptorSetLayoutBinding[mConvertedShader.mDescriptorSetLayoutBindingCount].binding = mConvertedShader.mDescriptorSetLayoutBindingCount;
			mConvertedShader.mDescriptorSetLayoutBinding[mConvertedShader.mDescriptorSetLayoutBindingCount].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
			mConvertedShader.mDescriptorSetLayoutBinding[mConvertedShader.mDescriptorSetLayoutBindingCount].descriptorCount = 1;
			mConvertedShader.mDescriptorSetLayoutBinding[mConvertedShader.mDescriptorSetLayoutBindingCount].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
			mConvertedShader.mDescriptorSetLayoutBinding[mConvertedShader.mDescriptorSetLayoutBindingCount].pImmutableSamplers = NULL;

			mConvertedShader.mDescriptorSetLayoutBindingCount++;

			BOOST_LOG_TRIVIAL(warning) << "Process_DCL - Unsupported declare type D3DSPR_SAMPLER";
		}
		else if (registerType == D3DSPR_INPUT)
		{
			mTypeInstructions.push_back(4); //size
			mTypeInstructions.push_back(spv::OpVariable); //opcode
			mTypeInstructions.push_back(GetSpirVTypeId(typeDescription)); //ResultType (Id) Must be OpTypePointer with the pointer's type being what you care about.
			mTypeInstructions.push_back(tokenId); //Result (Id)
			mTypeInstructions.push_back(spv::StorageClassInput); //Storage Class
			//Optional initializer
		}
		else if (registerType == D3DSPR_TEXTURE)
		{
			mTypeInstructions.push_back(4); //size
			mTypeInstructions.push_back(spv::OpVariable); //opcode
			mTypeInstructions.push_back(GetSpirVTypeId(spv::OpTypePointer, spv::OpTypeImage)); //ResultType (Id) Must be OpTypePointer with the pointer's type being what you care about.
			mTypeInstructions.push_back(tokenId); //Result (Id)
			mTypeInstructions.push_back(spv::StorageClassImage); //Storage Class
			//Optional initializer
		}
		else
		{
			BOOST_LOG_TRIVIAL(warning) << "Process_DCL - Unsupported declare type " << registerType;
		}
	}

}

void ShaderConverter::Process_DEF()
{
	Token token = GetNextToken();
	_D3DSHADER_PARAM_REGISTER_TYPE registerType = GetRegisterType(token.i);
	DestinationParameterToken  destinationParameterToken = token.DestinationParameterToken;

	uint32_t tokenId = GetNextVersionId(destinationParameterToken.RegisterNumber);
	TypeDescription typeDescription;
	typeDescription.PrimaryType = spv::OpTypeFloat;
	uint32_t resultTypeId = GetSpirVTypeId(typeDescription);
	mIdTypePairs[tokenId] = typeDescription;

	mTypeInstructions.push_back(7); //size
	mTypeInstructions.push_back(spv::OpConstant); //opcode
	mTypeInstructions.push_back(resultTypeId); //Result Type (Id)
	mTypeInstructions.push_back(tokenId); //Result (Id)
	for (size_t i = 0; i < 4; i++)
	{
		mTypeInstructions.push_back(GetNextToken().i); //Literal Values
	}
}

void ShaderConverter::Process_DEFI()
{
	Token token = GetNextToken();
	_D3DSHADER_PARAM_REGISTER_TYPE registerType = GetRegisterType(token.i);
	DestinationParameterToken  destinationParameterToken = token.DestinationParameterToken;

	uint32_t tokenId = GetNextVersionId(destinationParameterToken.RegisterNumber);
	TypeDescription typeDescription;
	typeDescription.PrimaryType = spv::OpTypeInt;
	uint32_t resultTypeId = GetSpirVTypeId(typeDescription);
	mIdTypePairs[tokenId] = typeDescription;

	mTypeInstructions.push_back(7); //size
	mTypeInstructions.push_back(spv::OpConstant); //opcode
	mTypeInstructions.push_back(resultTypeId); //Result Type (Id)
	mTypeInstructions.push_back(tokenId); //Result (Id)
	for (size_t i = 0; i < 4; i++)
	{
		mTypeInstructions.push_back(GetNextToken().i); //Literal Values
	}
}

void ShaderConverter::Process_DEFB()
{
	Token token = GetNextToken();
	_D3DSHADER_PARAM_REGISTER_TYPE registerType = GetRegisterType(token.i);
	DestinationParameterToken  destinationParameterToken = token.DestinationParameterToken;

	uint32_t tokenId = GetNextVersionId(destinationParameterToken.RegisterNumber);
	TypeDescription typeDescription;
	typeDescription.PrimaryType = spv::OpTypeBool;
	uint32_t resultTypeId = GetSpirVTypeId(typeDescription);
	mIdTypePairs[tokenId] = typeDescription;

	mTypeInstructions.push_back(7); //size
	mTypeInstructions.push_back(spv::OpConstant); //opcode
	mTypeInstructions.push_back(resultTypeId); //Result Type (Id)
	mTypeInstructions.push_back(tokenId); //Result (Id)
	for (size_t i = 0; i < 1; i++)
	{
		mTypeInstructions.push_back(GetNextToken().i); //Literal Values
	}
}

void ShaderConverter::Process_MOV()
{
	TypeDescription typeDescription;
	//spv::Op dataType;
	uint32_t dataTypeId;

	Token resultToken = GetNextToken();
	_D3DSHADER_PARAM_REGISTER_TYPE resultRegisterType = GetRegisterType(resultToken.i);

	Token argumentToken1 = GetNextToken();
	_D3DSHADER_PARAM_REGISTER_TYPE argumentRegisterType1 = GetRegisterType(argumentToken1.i);

	typeDescription = GetTypeByRegister(argumentToken1.DestinationParameterToken.RegisterNumber);
	dataTypeId = GetSpirVTypeId(typeDescription);

	mIdTypePairs[mNextId] = typeDescription; //snag next id before increment.

	mFunctionDefinitionInstructions.push_back(4); //count
	mFunctionDefinitionInstructions.push_back(spv::OpCopyObject); //Opcode
	mFunctionDefinitionInstructions.push_back(dataTypeId); //Result Type (Id)
	mFunctionDefinitionInstructions.push_back(GetNextVersionId(resultToken.DestinationParameterToken.RegisterNumber)); //result (Id)
	mFunctionDefinitionInstructions.push_back(mRegisterIdPairs[argumentToken1.DestinationParameterToken.RegisterNumber]); //argument1 (Id)
}

void ShaderConverter::Process_MUL()
{
	TypeDescription typeDescription;
	spv::Op dataType;
	uint32_t dataTypeId;

	Token resultToken = GetNextToken();
	_D3DSHADER_PARAM_REGISTER_TYPE resultRegisterType = GetRegisterType(resultToken.i);

	Token argumentToken1 = GetNextToken();
	_D3DSHADER_PARAM_REGISTER_TYPE argumentRegisterType1 = GetRegisterType(argumentToken1.i);

	Token argumentToken2 = GetNextToken();
	_D3DSHADER_PARAM_REGISTER_TYPE argumentRegisterType2 = GetRegisterType(argumentToken2.i);

	typeDescription = GetTypeByRegister(argumentToken1.DestinationParameterToken.RegisterNumber);
	dataTypeId = GetSpirVTypeId(typeDescription);

	mIdTypePairs[mNextId] = typeDescription; //snag next id before increment.

	if (typeDescription.PrimaryType == spv::OpTypeMatrix || typeDescription.PrimaryType == spv::OpTypeVector)
	{
		dataType = typeDescription.SecondaryType;
	}
	else
	{
		dataType = typeDescription.PrimaryType;
	}

	switch (dataType)
	{
	case spv::OpTypeBool:
		mFunctionDefinitionInstructions.push_back(5); //count
		mFunctionDefinitionInstructions.push_back(spv::OpIMul); //Opcode
		mFunctionDefinitionInstructions.push_back(dataTypeId); //Result Type (Id)
		mFunctionDefinitionInstructions.push_back(GetNextVersionId(resultToken.DestinationParameterToken.RegisterNumber)); //result (Id)
		mFunctionDefinitionInstructions.push_back(mRegisterIdPairs[argumentToken1.DestinationParameterToken.RegisterNumber]); //argument1 (Id)
		mFunctionDefinitionInstructions.push_back(mRegisterIdPairs[argumentToken2.DestinationParameterToken.RegisterNumber]); //argument2 (Id)
		break;
	case spv::OpTypeInt:
		mFunctionDefinitionInstructions.push_back(5); //count
		mFunctionDefinitionInstructions.push_back(spv::OpIMul); //Opcode
		mFunctionDefinitionInstructions.push_back(dataTypeId); //Result Type (Id)
		mFunctionDefinitionInstructions.push_back(GetNextVersionId(resultToken.DestinationParameterToken.RegisterNumber)); //result (Id)
		mFunctionDefinitionInstructions.push_back(mRegisterIdPairs[argumentToken1.DestinationParameterToken.RegisterNumber]); //argument1 (Id)
		mFunctionDefinitionInstructions.push_back(mRegisterIdPairs[argumentToken2.DestinationParameterToken.RegisterNumber]); //argument2 (Id)
		break;
	case spv::OpTypeFloat:
		mFunctionDefinitionInstructions.push_back(5); //count
		mFunctionDefinitionInstructions.push_back(spv::OpFMul); //Opcode
		mFunctionDefinitionInstructions.push_back(dataTypeId); //Result Type (Id)
		mFunctionDefinitionInstructions.push_back(GetNextVersionId(resultToken.DestinationParameterToken.RegisterNumber)); //result (Id)
		mFunctionDefinitionInstructions.push_back(mRegisterIdPairs[argumentToken1.DestinationParameterToken.RegisterNumber]); //argument1 (Id)
		mFunctionDefinitionInstructions.push_back(mRegisterIdPairs[argumentToken2.DestinationParameterToken.RegisterNumber]); //argument2 (Id)
		break;
	default:
		BOOST_LOG_TRIVIAL(warning) << "Process_MUL - Unsupported data type " << dataType;
		break;
	}
}

void ShaderConverter::Process_ADD()
{
	TypeDescription typeDescription;
	spv::Op dataType;
	uint32_t dataTypeId;

	Token resultToken = GetNextToken();
	_D3DSHADER_PARAM_REGISTER_TYPE resultRegisterType = GetRegisterType(resultToken.i);

	Token argumentToken1 = GetNextToken();
	_D3DSHADER_PARAM_REGISTER_TYPE argumentRegisterType1 = GetRegisterType(argumentToken1.i);

	Token argumentToken2 = GetNextToken();
	_D3DSHADER_PARAM_REGISTER_TYPE argumentRegisterType2 = GetRegisterType(argumentToken2.i);

	typeDescription = GetTypeByRegister(argumentToken1.DestinationParameterToken.RegisterNumber);
	dataTypeId = GetSpirVTypeId(typeDescription);

	mIdTypePairs[mNextId] = typeDescription; //snag next id before increment.

	if (typeDescription.PrimaryType == spv::OpTypeMatrix || typeDescription.PrimaryType == spv::OpTypeVector)
	{
		dataType = typeDescription.SecondaryType;
	}
	else
	{
		dataType = typeDescription.PrimaryType;
	}

	switch (dataType)
	{
	case spv::OpTypeBool:
		mFunctionDefinitionInstructions.push_back(5); //count
		mFunctionDefinitionInstructions.push_back(spv::OpIAdd); //Opcode
		mFunctionDefinitionInstructions.push_back(dataTypeId); //Result Type (Id)
		mFunctionDefinitionInstructions.push_back(GetNextVersionId(resultToken.DestinationParameterToken.RegisterNumber)); //result (Id)
		mFunctionDefinitionInstructions.push_back(mRegisterIdPairs[argumentToken1.DestinationParameterToken.RegisterNumber]); //argument1 (Id)
		mFunctionDefinitionInstructions.push_back(mRegisterIdPairs[argumentToken2.DestinationParameterToken.RegisterNumber]); //argument2 (Id)
		break;
	case spv::OpTypeInt:
		mFunctionDefinitionInstructions.push_back(5); //count
		mFunctionDefinitionInstructions.push_back(spv::OpIAdd); //Opcode
		mFunctionDefinitionInstructions.push_back(dataTypeId); //Result Type (Id)
		mFunctionDefinitionInstructions.push_back(GetNextVersionId(resultToken.DestinationParameterToken.RegisterNumber)); //result (Id)
		mFunctionDefinitionInstructions.push_back(mRegisterIdPairs[argumentToken1.DestinationParameterToken.RegisterNumber]); //argument1 (Id)
		mFunctionDefinitionInstructions.push_back(mRegisterIdPairs[argumentToken2.DestinationParameterToken.RegisterNumber]); //argument2 (Id)
		break;
	case spv::OpTypeFloat:
		mFunctionDefinitionInstructions.push_back(5); //count
		mFunctionDefinitionInstructions.push_back(spv::OpFAdd); //Opcode
		mFunctionDefinitionInstructions.push_back(dataTypeId); //Result Type (Id)
		mFunctionDefinitionInstructions.push_back(GetNextVersionId(resultToken.DestinationParameterToken.RegisterNumber)); //result (Id)
		mFunctionDefinitionInstructions.push_back(mRegisterIdPairs[argumentToken1.DestinationParameterToken.RegisterNumber]); //argument1 (Id)
		mFunctionDefinitionInstructions.push_back(mRegisterIdPairs[argumentToken2.DestinationParameterToken.RegisterNumber]); //argument2 (Id)
		break;
	default:
		BOOST_LOG_TRIVIAL(warning) << "Process_ADD - Unsupported data type " << dataType;
		break;
	}
}

void ShaderConverter::Process_SUB()
{
	TypeDescription typeDescription;
	spv::Op dataType;
	uint32_t dataTypeId;

	Token resultToken = GetNextToken();
	_D3DSHADER_PARAM_REGISTER_TYPE resultRegisterType = GetRegisterType(resultToken.i);

	Token argumentToken1 = GetNextToken();
	_D3DSHADER_PARAM_REGISTER_TYPE argumentRegisterType1 = GetRegisterType(argumentToken1.i);

	Token argumentToken2 = GetNextToken();
	_D3DSHADER_PARAM_REGISTER_TYPE argumentRegisterType2 = GetRegisterType(argumentToken2.i);

	typeDescription = GetTypeByRegister(argumentToken1.DestinationParameterToken.RegisterNumber);
	dataTypeId = GetSpirVTypeId(typeDescription);

	mIdTypePairs[mNextId] = typeDescription; //snag next id before increment.

	if (typeDescription.PrimaryType == spv::OpTypeMatrix || typeDescription.PrimaryType == spv::OpTypeVector)
	{
		dataType = typeDescription.SecondaryType;
	}
	else
	{
		dataType = typeDescription.PrimaryType;
	}

	switch (dataType)
	{
	case spv::OpTypeBool:
		mFunctionDefinitionInstructions.push_back(5); //count
		mFunctionDefinitionInstructions.push_back(spv::OpISub); //Opcode
		mFunctionDefinitionInstructions.push_back(dataTypeId); //Result Type (Id)
		mFunctionDefinitionInstructions.push_back(GetNextVersionId(resultToken.DestinationParameterToken.RegisterNumber)); //result (Id)
		mFunctionDefinitionInstructions.push_back(mRegisterIdPairs[argumentToken1.DestinationParameterToken.RegisterNumber]); //argument1 (Id)
		mFunctionDefinitionInstructions.push_back(mRegisterIdPairs[argumentToken2.DestinationParameterToken.RegisterNumber]); //argument2 (Id)
		break;
	case spv::OpTypeInt:
		mFunctionDefinitionInstructions.push_back(5); //count
		mFunctionDefinitionInstructions.push_back(spv::OpISub); //Opcode
		mFunctionDefinitionInstructions.push_back(dataTypeId); //Result Type (Id)
		mFunctionDefinitionInstructions.push_back(GetNextVersionId(resultToken.DestinationParameterToken.RegisterNumber)); //result (Id)
		mFunctionDefinitionInstructions.push_back(mRegisterIdPairs[argumentToken1.DestinationParameterToken.RegisterNumber]); //argument1 (Id)
		mFunctionDefinitionInstructions.push_back(mRegisterIdPairs[argumentToken2.DestinationParameterToken.RegisterNumber]); //argument2 (Id)
		break;
	case spv::OpTypeFloat:
		mFunctionDefinitionInstructions.push_back(5); //count
		mFunctionDefinitionInstructions.push_back(spv::OpFSub); //Opcode
		mFunctionDefinitionInstructions.push_back(dataTypeId); //Result Type (Id)
		mFunctionDefinitionInstructions.push_back(GetNextVersionId(resultToken.DestinationParameterToken.RegisterNumber)); //result (Id)
		mFunctionDefinitionInstructions.push_back(mRegisterIdPairs[argumentToken1.DestinationParameterToken.RegisterNumber]); //argument1 (Id)
		mFunctionDefinitionInstructions.push_back(mRegisterIdPairs[argumentToken2.DestinationParameterToken.RegisterNumber]); //argument2 (Id)
		break;
	default:
		BOOST_LOG_TRIVIAL(warning) << "Process_SUB - Unsupported data type " << dataType;
		break;
	}
}

void ShaderConverter::Process_MIN()
{
	BOOST_LOG_TRIVIAL(warning) << "Unsupported instruction D3DSIO_MIN.";
}

void ShaderConverter::Process_MAX()
{
	BOOST_LOG_TRIVIAL(warning) << "Unsupported instruction D3DSIO_MAX.";
}

void ShaderConverter::Process_DP3()
{
	//spv::Op dataType;
	uint32_t dataTypeId;

	Token resultToken = GetNextToken();
	_D3DSHADER_PARAM_REGISTER_TYPE resultRegisterType = GetRegisterType(resultToken.i);

	Token argumentToken1 = GetNextToken();
	_D3DSHADER_PARAM_REGISTER_TYPE argumentRegisterType1 = GetRegisterType(argumentToken1.i);

	Token argumentToken2 = GetNextToken();
	_D3DSHADER_PARAM_REGISTER_TYPE argumentRegisterType2 = GetRegisterType(argumentToken2.i);

	dataTypeId = GetSpirVTypeId(spv::OpTypeFloat);

	TypeDescription typeDescription;
	typeDescription.PrimaryType = spv::OpTypeFloat;
	mIdTypePairs[mNextId] = typeDescription; //snag next id before increment.

	mFunctionDefinitionInstructions.push_back(5); //count
	mFunctionDefinitionInstructions.push_back(spv::OpDot); //Opcode
	mFunctionDefinitionInstructions.push_back(dataTypeId); //Result Type (Id)
	mFunctionDefinitionInstructions.push_back(GetNextVersionId(resultToken.DestinationParameterToken.RegisterNumber)); //result (Id)
	mFunctionDefinitionInstructions.push_back(mRegisterIdPairs[argumentToken1.DestinationParameterToken.RegisterNumber]); //argument1 (Id)
	mFunctionDefinitionInstructions.push_back(mRegisterIdPairs[argumentToken2.DestinationParameterToken.RegisterNumber]); //argument2 (Id)
}

void ShaderConverter::Process_DP4()
{
	//spv::Op dataType;
	uint32_t dataTypeId;

	Token resultToken = GetNextToken();
	_D3DSHADER_PARAM_REGISTER_TYPE resultRegisterType = GetRegisterType(resultToken.i);

	Token argumentToken1 = GetNextToken();
	_D3DSHADER_PARAM_REGISTER_TYPE argumentRegisterType1 = GetRegisterType(argumentToken1.i);

	Token argumentToken2 = GetNextToken();
	_D3DSHADER_PARAM_REGISTER_TYPE argumentRegisterType2 = GetRegisterType(argumentToken2.i);

	dataTypeId = GetSpirVTypeId(spv::OpTypeFloat);

	TypeDescription typeDescription;
	typeDescription.PrimaryType = spv::OpTypeFloat;
	mIdTypePairs[mNextId] = typeDescription; //snag next id before increment.

	mFunctionDefinitionInstructions.push_back(5); //count
	mFunctionDefinitionInstructions.push_back(spv::OpDot); //Opcode
	mFunctionDefinitionInstructions.push_back(dataTypeId); //Result Type (Id)
	mFunctionDefinitionInstructions.push_back(GetNextVersionId(resultToken.DestinationParameterToken.RegisterNumber)); //result (Id)
	mFunctionDefinitionInstructions.push_back(mRegisterIdPairs[argumentToken1.DestinationParameterToken.RegisterNumber]); //argument1 (Id)
	mFunctionDefinitionInstructions.push_back(mRegisterIdPairs[argumentToken2.DestinationParameterToken.RegisterNumber]); //argument2 (Id)
}

void ShaderConverter::Process_TEX()
{
	//spv::Op dataType;
	uint32_t dataTypeId;

	Token resultToken = GetNextToken();
	_D3DSHADER_PARAM_REGISTER_TYPE resultRegisterType = GetRegisterType(resultToken.i);

	Token argumentToken1 = GetNextToken();
	_D3DSHADER_PARAM_REGISTER_TYPE argumentRegisterType1 = GetRegisterType(argumentToken1.i);

	Token argumentToken2 = GetNextToken();
	_D3DSHADER_PARAM_REGISTER_TYPE argumentRegisterType2 = GetRegisterType(argumentToken2.i);

	TypeDescription typeDescription;
	typeDescription.PrimaryType = spv::OpTypeVector;
	typeDescription.SecondaryType = spv::OpTypeFloat;
	typeDescription.ComponentCount = 4;
	mIdTypePairs[mNextId] = typeDescription; //snag next id before increment.

	dataTypeId = GetSpirVTypeId(typeDescription);

	mFunctionDefinitionInstructions.push_back(5); //count
	mFunctionDefinitionInstructions.push_back(spv::OpImageFetch); //Opcode
	mFunctionDefinitionInstructions.push_back(dataTypeId); //Result Type (Id)
	mFunctionDefinitionInstructions.push_back(GetNextVersionId(resultToken.DestinationParameterToken.RegisterNumber)); //result (Id)
	mFunctionDefinitionInstructions.push_back(mRegisterIdPairs[argumentToken2.DestinationParameterToken.RegisterNumber]); //argument2 (Id)
	mFunctionDefinitionInstructions.push_back(mRegisterIdPairs[argumentToken1.DestinationParameterToken.RegisterNumber]); //argument1 (Id)	
}

void ShaderConverter::Process_MAD()
{
	TypeDescription typeDescription;
	spv::Op dataType;
	uint32_t dataTypeId;
	uint32_t resultId;

	Token resultToken = GetNextToken();
	_D3DSHADER_PARAM_REGISTER_TYPE resultRegisterType = GetRegisterType(resultToken.i);

	Token argumentToken1 = GetNextToken();
	_D3DSHADER_PARAM_REGISTER_TYPE argumentRegisterType1 = GetRegisterType(argumentToken1.i);

	Token argumentToken2 = GetNextToken();
	_D3DSHADER_PARAM_REGISTER_TYPE argumentRegisterType2 = GetRegisterType(argumentToken2.i);

	Token argumentToken3 = GetNextToken();
	_D3DSHADER_PARAM_REGISTER_TYPE argumentRegisterType3 = GetRegisterType(argumentToken3.i);

	dataTypeId = GetSpirVTypeId(spv::OpTypeFloat);

	mIdTypePairs[mNextId] = typeDescription; //snag next id before increment.

	if (typeDescription.PrimaryType == spv::OpTypeMatrix || typeDescription.PrimaryType == spv::OpTypeVector)
	{
		dataType = typeDescription.SecondaryType;
	}
	else
	{
		dataType = typeDescription.PrimaryType;
	}

	resultId = GetNextVersionId(resultToken.DestinationParameterToken.RegisterNumber);

	switch (dataType)
	{
	case spv::OpTypeBool:
		
		//Write out multiply
		mFunctionDefinitionInstructions.push_back(5); //count
		mFunctionDefinitionInstructions.push_back(spv::OpIMul); //Opcode
		mFunctionDefinitionInstructions.push_back(dataTypeId); //Result Type (Id)
		mFunctionDefinitionInstructions.push_back(resultId); //result (Id)
		mFunctionDefinitionInstructions.push_back(mRegisterIdPairs[argumentToken1.DestinationParameterToken.RegisterNumber]); //argument1 (Id)
		mFunctionDefinitionInstructions.push_back(mRegisterIdPairs[argumentToken2.DestinationParameterToken.RegisterNumber]); //argument2 (Id)

		//Write out add
		mFunctionDefinitionInstructions.push_back(5); //count
		mFunctionDefinitionInstructions.push_back(spv::OpIAdd); //Opcode
		mFunctionDefinitionInstructions.push_back(dataTypeId); //Result Type (Id)
		mFunctionDefinitionInstructions.push_back(GetNextVersionId(resultToken.DestinationParameterToken.RegisterNumber)); //result (Id)
		mFunctionDefinitionInstructions.push_back(resultId); //argument1 (Id)
		mFunctionDefinitionInstructions.push_back(mRegisterIdPairs[argumentToken3.DestinationParameterToken.RegisterNumber]); //argument2 (Id)

		break;
	case spv::OpTypeInt:
		//Write out multiply
		mFunctionDefinitionInstructions.push_back(5); //count
		mFunctionDefinitionInstructions.push_back(spv::OpIMul); //Opcode
		mFunctionDefinitionInstructions.push_back(dataTypeId); //Result Type (Id)
		mFunctionDefinitionInstructions.push_back(resultId); //result (Id)
		mFunctionDefinitionInstructions.push_back(mRegisterIdPairs[argumentToken1.DestinationParameterToken.RegisterNumber]); //argument1 (Id)
		mFunctionDefinitionInstructions.push_back(mRegisterIdPairs[argumentToken2.DestinationParameterToken.RegisterNumber]); //argument2 (Id)

		//Write out add
		mFunctionDefinitionInstructions.push_back(5); //count
		mFunctionDefinitionInstructions.push_back(spv::OpIAdd); //Opcode
		mFunctionDefinitionInstructions.push_back(dataTypeId); //Result Type (Id)
		mFunctionDefinitionInstructions.push_back(GetNextVersionId(resultToken.DestinationParameterToken.RegisterNumber)); //result (Id)
		mFunctionDefinitionInstructions.push_back(resultId); //argument1 (Id)
		mFunctionDefinitionInstructions.push_back(mRegisterIdPairs[argumentToken3.DestinationParameterToken.RegisterNumber]); //argument2 (Id)

		break;
	case spv::OpTypeFloat:
		//Write out multiply
		mFunctionDefinitionInstructions.push_back(5); //count
		mFunctionDefinitionInstructions.push_back(spv::OpFMul); //Opcode
		mFunctionDefinitionInstructions.push_back(dataTypeId); //Result Type (Id)
		mFunctionDefinitionInstructions.push_back(resultId); //result (Id)
		mFunctionDefinitionInstructions.push_back(mRegisterIdPairs[argumentToken1.DestinationParameterToken.RegisterNumber]); //argument1 (Id)
		mFunctionDefinitionInstructions.push_back(mRegisterIdPairs[argumentToken2.DestinationParameterToken.RegisterNumber]); //argument2 (Id)

		//Write out add
		mFunctionDefinitionInstructions.push_back(5); //count
		mFunctionDefinitionInstructions.push_back(spv::OpFAdd); //Opcode
		mFunctionDefinitionInstructions.push_back(dataTypeId); //Result Type (Id)
		mFunctionDefinitionInstructions.push_back(GetNextVersionId(resultToken.DestinationParameterToken.RegisterNumber)); //result (Id)
		mFunctionDefinitionInstructions.push_back(resultId); //argument1 (Id)
		mFunctionDefinitionInstructions.push_back(mRegisterIdPairs[argumentToken3.DestinationParameterToken.RegisterNumber]); //argument2 (Id)

		break;
	default:
		BOOST_LOG_TRIVIAL(warning) << "Process_SUB - Unsupported data type " << dataType;
		break;
	}
}

ConvertedShader ShaderConverter::Convert(uint32_t* shader)
{
	mConvertedShader = {};
	mInstructions.clear();

	uint32_t token;
	uint32_t instruction;
	mBaseToken = mNextToken = shader;

	token = GetNextToken().i;
	mMajorVersion = D3DSHADER_VERSION_MAJOR(token);
	mMinorVersion = D3DSHADER_VERSION_MINOR(token);

	if ((token & 0xFFFF0000) == 0xFFFF0000)
	{
		mIsVertexShader = false;
	}
	else
	{
		mIsVertexShader = true;
	}
	//Probably more info in this word but I'll handle that later.

	//End of entry point
	mEntryPointTypeId = GetSpirVTypeId(spv::OpFunction); //secondary type will be void by default.
	mEntryPointId = GetNextId();

	mFunctionDefinitionInstructions.push_back(5); //count
	mFunctionDefinitionInstructions.push_back(spv::OpFunction); //Opcode
	mFunctionDefinitionInstructions.push_back(GetSpirVTypeId(spv::OpTypeVoid)); //Result Type (Id)
	mFunctionDefinitionInstructions.push_back(mEntryPointId); //result (Id)
	mFunctionDefinitionInstructions.push_back(spv::FunctionControlMaskNone); //Function Control
	mFunctionDefinitionInstructions.push_back(mEntryPointTypeId); //Function Type (Id)

	//Read DXBC instructions
	while (token != D3DPS_END())
	{
		mTokenOffset = mNextToken - shader;
		token = GetNextToken().i;
		instruction = GetOpcode(token);

		switch (instruction)
		{
		case D3DSIO_NOP:
			//Nothing
			break;
		case D3DSIO_PHASE:
			BOOST_LOG_TRIVIAL(warning) << "Unsupported instruction D3DSIO_PHASE.";
			break;
		case D3DSIO_RET:
			BOOST_LOG_TRIVIAL(warning) << "Unsupported instruction D3DSIO_RET.";
			break;
		case D3DSIO_ENDLOOP:
			BOOST_LOG_TRIVIAL(warning) << "Unsupported instruction D3DSIO_ENDLOOP.";
			break;
		case D3DSIO_BREAK:
			BOOST_LOG_TRIVIAL(warning) << "Unsupported instruction D3DSIO_BREAK.";
			break;
		case D3DSIO_TEXDEPTH:
			BOOST_LOG_TRIVIAL(warning) << "Unsupported instruction D3DSIO_TEXDEPTH.";
			break;
		case D3DSIO_TEXKILL:
			BOOST_LOG_TRIVIAL(warning) << "Unsupported instruction D3DSIO_TEXKILL.";
			break;
		case D3DSIO_BEM:
			BOOST_LOG_TRIVIAL(warning) << "Unsupported instruction D3DSIO_BEM.";
			break;
		case D3DSIO_TEXBEM:
			BOOST_LOG_TRIVIAL(warning) << "Unsupported instruction D3DSIO_TEXBEM.";
			break;
		case D3DSIO_TEXBEML:
			BOOST_LOG_TRIVIAL(warning) << "Unsupported instruction D3DSIO_TEXBEML.";
			break;
		case D3DSIO_TEXDP3:
			BOOST_LOG_TRIVIAL(warning) << "Unsupported instruction D3DSIO_TEXDP3.";
			break;
		case D3DSIO_TEXDP3TEX:
			BOOST_LOG_TRIVIAL(warning) << "Unsupported instruction D3DSIO_TEXDP3TEX.";
			break;
		case D3DSIO_TEXM3x2DEPTH:
			BOOST_LOG_TRIVIAL(warning) << "Unsupported instruction D3DSIO_TEXM3x2DEPTH.";
			break;
		case D3DSIO_TEXM3x2TEX:
			BOOST_LOG_TRIVIAL(warning) << "Unsupported instruction D3DSIO_TEXM3x2TEX.";
			break;
		case D3DSIO_TEXM3x3:
			BOOST_LOG_TRIVIAL(warning) << "Unsupported instruction D3DSIO_TEXM3x3.";
			break;
		case D3DSIO_TEXM3x3PAD:
			BOOST_LOG_TRIVIAL(warning) << "Unsupported instruction D3DSIO_TEXM3x3PAD.";
			break;
		case D3DSIO_TEXM3x3TEX:
			BOOST_LOG_TRIVIAL(warning) << "Unsupported instruction D3DSIO_TEXM3x3TEX.";
			break;
		case D3DSIO_TEXM3x3VSPEC:
			BOOST_LOG_TRIVIAL(warning) << "Unsupported instruction D3DSIO_TEXM3x3VSPEC.";
			break;
		case D3DSIO_TEXREG2AR:
			BOOST_LOG_TRIVIAL(warning) << "Unsupported instruction D3DSIO_TEXREG2AR.";
			break;
		case D3DSIO_TEXREG2GB:
			BOOST_LOG_TRIVIAL(warning) << "Unsupported instruction D3DSIO_TEXREG2GB.";
			break;
		case D3DSIO_TEXREG2RGB:
			BOOST_LOG_TRIVIAL(warning) << "Unsupported instruction D3DSIO_TEXREG2RGB.";
			break;
		case D3DSIO_LABEL:
			BOOST_LOG_TRIVIAL(warning) << "Unsupported instruction D3DSIO_LABEL.";
			break;
		case D3DSIO_CALL:
			BOOST_LOG_TRIVIAL(warning) << "Unsupported instruction D3DSIO_CALL.";
			break;
		case D3DSIO_LOOP:
			BOOST_LOG_TRIVIAL(warning) << "Unsupported instruction D3DSIO_LOOP.";
			break;
		case D3DSIO_BREAKP:
			BOOST_LOG_TRIVIAL(warning) << "Unsupported instruction D3DSIO_BREAKP.";
			break;
		case D3DSIO_DSX:
			BOOST_LOG_TRIVIAL(warning) << "Unsupported instruction D3DSIO_DSX.";
			break;
		case D3DSIO_DSY:
			BOOST_LOG_TRIVIAL(warning) << "Unsupported instruction D3DSIO_DSY.";
			break;
		case D3DSIO_IFC:
			BOOST_LOG_TRIVIAL(warning) << "Unsupported instruction D3DSIO_IFC.";
			break;
		case D3DSIO_IF:
			BOOST_LOG_TRIVIAL(warning) << "Unsupported instruction D3DSIO_IF.";
			break;
		case D3DSIO_ELSE:
			BOOST_LOG_TRIVIAL(warning) << "Unsupported instruction D3DSIO_ELSE.";
			break;
		case D3DSIO_ENDIF:
			BOOST_LOG_TRIVIAL(warning) << "Unsupported instruction D3DSIO_ENDIF.";
			break;
		case D3DSIO_REP:
			BOOST_LOG_TRIVIAL(warning) << "Unsupported instruction D3DSIO_REP.";
			break;
		case D3DSIO_ENDREP:
			BOOST_LOG_TRIVIAL(warning) << "Unsupported instruction D3DSIO_ENDREP.";
			break;
		case D3DSIO_NRM:
			BOOST_LOG_TRIVIAL(warning) << "Unsupported instruction D3DSIO_NRM.";
			break;
		case D3DSIO_MOVA:
			BOOST_LOG_TRIVIAL(warning) << "Unsupported instruction D3DSIO_MOVA.";
			break;
		case D3DSIO_MOV:
			Process_MOV();
			break;
		case D3DSIO_RCP:
			BOOST_LOG_TRIVIAL(warning) << "Unsupported instruction D3DSIO_RCP.";
			break;
		case D3DSIO_RSQ:
			BOOST_LOG_TRIVIAL(warning) << "Unsupported instruction D3DSIO_RSQ.";
			break;
		case D3DSIO_EXP:
			BOOST_LOG_TRIVIAL(warning) << "Unsupported instruction D3DSIO_EXP.";
			break;
		case D3DSIO_EXPP:
			BOOST_LOG_TRIVIAL(warning) << "Unsupported instruction D3DSIO_EXPP.";
			break;
		case D3DSIO_LOG:
			BOOST_LOG_TRIVIAL(warning) << "Unsupported instruction D3DSIO_LOG.";
			break;
		case D3DSIO_LOGP:
			BOOST_LOG_TRIVIAL(warning) << "Unsupported instruction D3DSIO_LOGP.";
			break;
		case D3DSIO_FRC:
			BOOST_LOG_TRIVIAL(warning) << "Unsupported instruction D3DSIO_FRC.";
			break;
		case D3DSIO_LIT:
			BOOST_LOG_TRIVIAL(warning) << "Unsupported instruction D3DSIO_LIT.";
			break;
		case D3DSIO_ABS:
			BOOST_LOG_TRIVIAL(warning) << "Unsupported instruction D3DSIO_ABS.";
			break;
		case D3DSIO_TEXM3x3SPEC:
			BOOST_LOG_TRIVIAL(warning) << "Unsupported instruction D3DSIO_TEXM3x3SPEC.";
			break;
		case D3DSIO_M4x4:
			BOOST_LOG_TRIVIAL(warning) << "Unsupported instruction D3DSIO_M4x4.";
			break;
		case D3DSIO_M4x3:
			BOOST_LOG_TRIVIAL(warning) << "Unsupported instruction D3DSIO_M4x3.";
			break;
		case D3DSIO_M3x4:
			BOOST_LOG_TRIVIAL(warning) << "Unsupported instruction D3DSIO_M3x4.";
			break;
		case D3DSIO_M3x3:
			BOOST_LOG_TRIVIAL(warning) << "Unsupported instruction D3DSIO_M3x3.";
			break;
		case D3DSIO_M3x2:
			BOOST_LOG_TRIVIAL(warning) << "Unsupported instruction D3DSIO_M3x2.";
			break;
		case D3DSIO_CALLNZ:
			BOOST_LOG_TRIVIAL(warning) << "Unsupported instruction D3DSIO_CALLNZ.";
			break;
		case D3DSIO_SETP:
			BOOST_LOG_TRIVIAL(warning) << "Unsupported instruction D3DSIO_SETP.";
			break;
		case D3DSIO_BREAKC:
			BOOST_LOG_TRIVIAL(warning) << "Unsupported instruction D3DSIO_BREAKC.";
			break;
		case D3DSIO_ADD:
			Process_ADD();
			break;
		case D3DSIO_SUB:
			Process_SUB();
			break;
		case D3DSIO_MUL:
			Process_MUL();
			break;
		case D3DSIO_DP3:
			Process_DP3();
			break;
		case D3DSIO_DP4:
			Process_DP4();
			break;
		case D3DSIO_MIN:
			Process_MIN();
			break;
		case D3DSIO_MAX:
			Process_MAX();
			break;
		case D3DSIO_DST:
			BOOST_LOG_TRIVIAL(warning) << "Unsupported instruction D3DSIO_DST.";
			break;
		case D3DSIO_SLT:
			BOOST_LOG_TRIVIAL(warning) << "Unsupported instruction D3DSIO_SLT.";
			break;
		case D3DSIO_SGE:
			BOOST_LOG_TRIVIAL(warning) << "Unsupported instruction D3DSIO_SGE.";
			break;
		case D3DSIO_CRS:
			BOOST_LOG_TRIVIAL(warning) << "Unsupported instruction D3DSIO_CRS.";
			break;
		case D3DSIO_POW:
			BOOST_LOG_TRIVIAL(warning) << "Unsupported instruction D3DSIO_POW.";
			break;
		case D3DSIO_DP2ADD:
			BOOST_LOG_TRIVIAL(warning) << "Unsupported instruction D3DSIO_DP2ADD.";
			break;
		case D3DSIO_LRP:
			BOOST_LOG_TRIVIAL(warning) << "Unsupported instruction D3DSIO_LRP.";
			break;
		case D3DSIO_SGN:
			BOOST_LOG_TRIVIAL(warning) << "Unsupported instruction D3DSIO_SGN.";
			break;
		case D3DSIO_CND:
			BOOST_LOG_TRIVIAL(warning) << "Unsupported instruction D3DSIO_CND.";
			break;
		case D3DSIO_CMP:
			BOOST_LOG_TRIVIAL(warning) << "Unsupported instruction D3DSIO_CMP.";
			break;
		case D3DSIO_SINCOS:
			BOOST_LOG_TRIVIAL(warning) << "Unsupported instruction D3DSIO_SINCOS.";
			break;
		case D3DSIO_MAD:
			Process_MAD();
			break;
		case D3DSIO_TEXLDD:
			BOOST_LOG_TRIVIAL(warning) << "Unsupported instruction D3DSIO_TEXLDD.";
			break;
		case D3DSIO_TEXCOORD:
			BOOST_LOG_TRIVIAL(warning) << "Unsupported instruction D3DSIO_TEXCOORD.";
			break;
		case D3DSIO_TEX:
			Process_TEX();
			break;
		case D3DSIO_TEXLDL:
			BOOST_LOG_TRIVIAL(warning) << "Unsupported instruction D3DSIO_TEXLDL.";
			break;
		case D3DSIO_DCL:
			Process_DCL();
			break;
		case D3DSIO_DEFB:
			Process_DEFB();
			break;
		case D3DSIO_DEFI:
			Process_DEFI();
			break;
		case D3DSIO_DEF:
			Process_DEF();
			break;
		case D3DSIO_COMMENT:
			SkipTokens(((token & 0x0fff0000) >> 16));
			break;
		case D3DSIO_END:
			//Nothing
			break;
		default:
			BOOST_LOG_TRIVIAL(warning) << "Unsupported instruction " << instruction << ".";
			break;
		}

	}

	//End of entry point
	mFunctionDefinitionInstructions.push_back(1); //count
	mFunctionDefinitionInstructions.push_back(spv::OpReturn); //Opcode
	mFunctionDefinitionInstructions.push_back(1); //count
	mFunctionDefinitionInstructions.push_back(spv::OpFunctionEnd); //Opcode

	//Capability
	mCapabilityInstructions.push_back(2); //count
	mCapabilityInstructions.push_back(spv::OpCapability); //Opcode
	mCapabilityInstructions.push_back(spv::CapabilityShader); //Capability

	//Import
	std::string importStatement = "GLSL.std.450";
	//The spec says 3+variable but there are only 2 before string literal.
	mExtensionInstructions.push_back(2 + importStatement.length()/4); //count
	mExtensionInstructions.push_back(spv::OpExtInstImport); //Opcode
	mExtensionInstructions.push_back(GetNextId()); //Result Id
	PutStringInVector(importStatement, mExtensionInstructions);

	//Memory Model
	mMemoryModelInstructions.push_back(3); //count
	mMemoryModelInstructions.push_back(spv::OpMemoryModel); //Opcode
	mMemoryModelInstructions.push_back(spv::AddressingModelLogical); //Addressing Model
	mMemoryModelInstructions.push_back(spv::MemoryModelGLSL450); //Memory Model

	//EntryPoint
	std::string entryPointName = "main";
	//The spec says 4+variable but there are only 3 before the string literal.
	mEntryPointInstructions.push_back(3 + (entryPointName.length()/4) + mInterfaceIds.size()); //count
	mEntryPointInstructions.push_back(spv::OpEntryPoint); //Opcode
	if (mIsVertexShader)
	{
		mEntryPointInstructions.push_back(spv::ExecutionModelVertex); //Execution Model
	}
	else
	{
		mEntryPointInstructions.push_back(spv::ExecutionModelFragment); //Execution Model
	}
	mEntryPointInstructions.push_back(mEntryPointId); //Entry Point (Id)
	PutStringInVector(entryPointName, mEntryPointInstructions); //Name
	mEntryPointInstructions.insert(std::end(mEntryPointInstructions), std::begin(mInterfaceIds), std::end(mInterfaceIds)); //Interfaces
	
	//ExecutionMode
	if (!mIsVertexShader)
	{
		mExecutionModeInstructions.push_back(3); //count
		mExecutionModeInstructions.push_back(spv::OpExecutionMode); //Opcode
		mExecutionModeInstructions.push_back(mEntryPointId); //Entry Point (Id)
		mExecutionModeInstructions.push_back(spv::ExecutionModeOriginLowerLeft); //Execution Mode
	}
	else
	{
		//TODO: figure out what to do for vertex execution mode.
	}

	//Write SPIR-V header
	mInstructions.push_back(spv::MagicNumber);
	mInstructions.push_back(spv::Version);
	mInstructions.push_back(0); //I don't have a generator number ATM.
	mInstructions.push_back(GetNextId()); //Bound
	mInstructions.push_back(0); //Reserved for instruction schema, if needed

	//Dump other opcodes into instruction collection is required order.
	CombineSpirVOpCodes();

	//Pass the word blob to Vulkan to generate a module.
	CreateSpirVModule();

	return mConvertedShader; //Return value optimization don't fail me now.
}
