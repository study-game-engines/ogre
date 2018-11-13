/*
-----------------------------------------------------------------------------
This source file is part of OGRE
(Object-oriented Graphics Rendering Engine)
For the latest info, see http://www.ogre3d.org

Copyright (c) 2000-2014 Torus Knot Software Ltd
Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
-----------------------------------------------------------------------------
*/
#include "OgreShaderPrecompiledHeaders.h"
#ifdef RTSHADER_SYSTEM_BUILD_EXT_SHADERS

namespace Ogre {
namespace RTShader {

    String TriplanarTexturing::type = "SGX_TriplanarTexturing";

    //-----------------------------------------------------------------------

	bool TriplanarTexturing::resolveParameters(ProgramSet* programSet)
	{
		Program* vsProgram = programSet->getCpuProgram(GPT_VERTEX_PROGRAM);
		Program* psProgram = programSet->getCpuProgram(GPT_FRAGMENT_PROGRAM);
		Function* vsMain = vsProgram->getEntryPointFunction();
		Function* psMain = psProgram->getEntryPointFunction();

		// Resolve pixel shader output diffuse color.
		mPSInDiffuse = vsMain->resolveInputParameter(Parameter::SPC_COLOR_DIFFUSE);

		// Resolve input vertex shader normal.
		mVSInNormal = vsMain->resolveInputParameter(Parameter::SPC_NORMAL_OBJECT_SPACE);

		// Resolve output vertex shader normal.
		mVSOutNormal = vsMain->resolveOutputParameter(Parameter::SPS_TEXTURE_COORDINATES, -1, Parameter::SPC_NORMAL_VIEW_SPACE, GCT_FLOAT3);

		// Resolve pixel shader output diffuse color.
		mPSInDiffuse = psMain->resolveInputParameter(Parameter::SPC_COLOR_DIFFUSE);

		// Resolve input pixel shader normal.
		mPSInNormal = psMain->resolveInputParameter(Parameter::SPS_TEXTURE_COORDINATES,
			mVSOutNormal->getIndex(),
			mVSOutNormal->getContent(),
			GCT_FLOAT3);

		// Resolve input vertex shader normal.
		mVSInPosition = vsMain->resolveInputParameter(Parameter::SPC_POSITION_OBJECT_SPACE);

		mVSOutPosition = vsMain->resolveOutputParameter(Parameter::SPS_TEXTURE_COORDINATES, -1, Parameter::SPC_POSITION_OBJECT_SPACE, GCT_FLOAT4);
		mPSInPosition = psMain->resolveInputParameter(Parameter::SPS_TEXTURE_COORDINATES,
			mVSOutPosition->getIndex(),
			mVSOutPosition->getContent(),
			GCT_FLOAT4);

		mSamplerFromX = psProgram->resolveParameter(GCT_SAMPLER2D, mTextureSamplerIndexFromX, (uint16)GPV_GLOBAL, "tp_sampler_from_x");
		if (mSamplerFromX.get() == NULL)
			return false;

		mSamplerFromY = psProgram->resolveParameter(GCT_SAMPLER2D, mTextureSamplerIndexFromY, (uint16)GPV_GLOBAL, "tp_sampler_from_y");
		if (mSamplerFromY.get() == NULL)
			return false;

		mSamplerFromZ = psProgram->resolveParameter(GCT_SAMPLER2D, mTextureSamplerIndexFromZ, (uint16)GPV_GLOBAL, "tp_sampler_from_z");
		if (mSamplerFromZ.get() == NULL)
			return false;

        mPSOutDiffuse = psMain->resolveOutputParameter(Parameter::SPC_COLOR_DIFFUSE);
        if (mPSOutDiffuse.get() == NULL)    
            return false;
    
        mPSTPParams = psProgram->resolveParameter(GCT_FLOAT3, -1, (uint16)GPV_GLOBAL, "gTPParams");
        if (mPSTPParams.get() == NULL)
            return false;
        return true;
    }

    //-----------------------------------------------------------------------
    bool TriplanarTexturing::resolveDependencies(ProgramSet* programSet)
    {
        Program* psProgram = programSet->getCpuProgram(GPT_FRAGMENT_PROGRAM);
        Program* vsProgram = programSet->getCpuProgram(GPT_VERTEX_PROGRAM);
		psProgram->addDependency(FFP_LIB_TEXTURING);
        psProgram->addDependency("SGXLib_TriplanarTexturing");
        vsProgram->addDependency(FFP_LIB_COMMON);
        return true;
    }

    //-----------------------------------------------------------------------
	bool TriplanarTexturing::addFunctionInvocations(ProgramSet* programSet)
	{
        Program* psProgram = programSet->getCpuProgram(GPT_FRAGMENT_PROGRAM);
        Function* psMain = psProgram->getEntryPointFunction();
        Program* vsProgram = programSet->getCpuProgram(GPT_VERTEX_PROGRAM);
        Function* vsMain = vsProgram->getEntryPointFunction();

        auto vsStage = vsMain->getStage(FFP_PS_TEXTURING);
        vsStage.assign(mVSInNormal, mVSOutNormal);
        vsStage.assign(mVSInPosition, mVSOutPosition);

        psMain->getStage(FFP_PS_TEXTURING)
            .callFunction(SGX_FUNC_TRIPLANAR_TEXTURING,
                          {In(mPSInDiffuse), In(mPSInNormal), In(mPSInPosition), In(mSamplerFromX),
                           In(mSamplerFromY), In(mSamplerFromZ), In(mPSTPParams), Out(mPSOutDiffuse)});

        return true;
    }

    //-----------------------------------------------------------------------
    const String& TriplanarTexturing::getType() const
    {
        return type;
    }

    //-----------------------------------------------------------------------
    int TriplanarTexturing::getExecutionOrder() const
    {
        return FFP_TEXTURING;
    }

    //-----------------------------------------------------------------------
    bool TriplanarTexturing::preAddToRenderState(const RenderState* renderState, Pass* srcPass, Pass* dstPass )
    {
        TextureUnitState* textureUnit;
    
        // Create the mapping textures
        textureUnit = dstPass->createTextureUnitState();
        textureUnit->setTextureName(mTextureNameFromX);     
        mTextureSamplerIndexFromX = dstPass->getNumTextureUnitStates() - 1;
    
        textureUnit = dstPass->createTextureUnitState();
        textureUnit->setTextureName(mTextureNameFromY);     
        mTextureSamplerIndexFromY = dstPass->getNumTextureUnitStates() - 1;
    
        textureUnit = dstPass->createTextureUnitState();
        textureUnit->setTextureName(mTextureNameFromZ);     
        mTextureSamplerIndexFromZ = dstPass->getNumTextureUnitStates() - 1;
        return true;
    }

    //-----------------------------------------------------------------------
    void TriplanarTexturing::copyFrom(const SubRenderState& rhs)
    {
        const TriplanarTexturing& rhsTP = static_cast<const TriplanarTexturing&>(rhs);
    
        mPSOutDiffuse = rhsTP.mPSOutDiffuse;
        mPSInDiffuse = rhsTP.mPSInDiffuse;

        mVSInPosition = rhsTP.mVSInPosition;
        mVSOutPosition = rhsTP.mVSOutPosition;

        mVSOutNormal = rhsTP.mVSOutNormal;
        mVSInNormal = rhsTP.mVSInNormal;
        mPSInNormal = rhsTP.mPSInNormal;

        mVSOutPosition = rhsTP.mVSOutPosition;
        mVSInPosition = rhsTP.mVSInPosition;
        mPSInPosition = rhsTP.mPSInPosition;

        mSamplerFromX = rhsTP.mSamplerFromX;
        mSamplerFromY = rhsTP.mSamplerFromY;
        mSamplerFromZ = rhsTP.mSamplerFromZ;

        mPSTPParams = rhsTP.mPSTPParams;
        mParameters = rhsTP.mParameters;
        mTextureNameFromX = rhsTP.mTextureNameFromX;
        mTextureNameFromY = rhsTP.mTextureNameFromY;
        mTextureNameFromZ = rhsTP.mTextureNameFromZ;
    }

    //-----------------------------------------------------------------------
    void TriplanarTexturing::updateGpuProgramsParams(Renderable* rend, Pass* pass, const AutoParamDataSource* source, 
                                         const LightList* pLightList)
    {
        mPSTPParams->setGpuParameter(mParameters);
    }

    //-----------------------------------------------------------------------
    void TriplanarTexturing::setParameters(const Vector3 &parameters)
    {
        mParameters = parameters;
    }

    //-----------------------------------------------------------------------
    void TriplanarTexturing::setTextureNames(const String &textureNameFromX, const String &textureNameFromY, const String &textureNameFromZ)
    {
        mTextureNameFromX = textureNameFromX;
        mTextureNameFromY = textureNameFromY;
        mTextureNameFromZ = textureNameFromZ;
    }

    //-----------------------------------------------------------------------
    const String& TriplanarTexturingFactory::getType() const
    {
        return TriplanarTexturing::type;
    }

    //-----------------------------------------------------------------------
    SubRenderState* TriplanarTexturingFactory::createInstance(ScriptCompiler* compiler, 
                                                       PropertyAbstractNode* prop, Pass* pass, SGScriptTranslator* translator)
    {
        if (prop->name == "triplanarTexturing")
        {
            if (prop->values.size() == 6)
            {
                SubRenderState* subRenderState = createOrRetrieveInstance(translator);
                TriplanarTexturing* tpSubRenderState = static_cast<TriplanarTexturing*>(subRenderState);
                
                AbstractNodeList::const_iterator it = prop->values.begin();
                float parameters[3];
                if (false == SGScriptTranslator::getFloat(*it, parameters))
                {
                    compiler->addError(ScriptCompiler::CE_INVALIDPARAMETERS, prop->file, prop->line);
                    return NULL;
                }
                ++it;
                if (false == SGScriptTranslator::getFloat(*it, parameters + 1))
                {
                    compiler->addError(ScriptCompiler::CE_INVALIDPARAMETERS, prop->file, prop->line);
                    return NULL;
                }
                ++it;
                if (false == SGScriptTranslator::getFloat(*it, parameters + 2))
                {
                    compiler->addError(ScriptCompiler::CE_INVALIDPARAMETERS, prop->file, prop->line);
                    return NULL;
                }
                Vector3 vParameters(parameters[0], parameters[1], parameters[2]);
                tpSubRenderState->setParameters(vParameters);

                String textureNameFromX, textureNameFromY, textureNameFromZ;
                ++it;
                if (false == SGScriptTranslator::getString(*it, &textureNameFromX))
                {
                    compiler->addError(ScriptCompiler::CE_INVALIDPARAMETERS, prop->file, prop->line);
                    return NULL;
                }
                ++it;
                if (false == SGScriptTranslator::getString(*it, &textureNameFromY))
                {
                    compiler->addError(ScriptCompiler::CE_INVALIDPARAMETERS, prop->file, prop->line);
                    return NULL;
                }
                ++it;
                if (false == SGScriptTranslator::getString(*it, &textureNameFromZ))
                {
                    compiler->addError(ScriptCompiler::CE_INVALIDPARAMETERS, prop->file, prop->line);
                    return NULL;
                }
                tpSubRenderState->setTextureNames(textureNameFromX, textureNameFromY, textureNameFromZ);

                return subRenderState;
            }
            else
            {
                compiler->addError(ScriptCompiler::CE_INVALIDPARAMETERS, prop->file, prop->line);
            }
        }
        return NULL;
    }

    //-----------------------------------------------------------------------
    SubRenderState* TriplanarTexturingFactory::createInstanceImpl()
    {
        return OGRE_NEW TriplanarTexturing;
    }


}
}

#endif
