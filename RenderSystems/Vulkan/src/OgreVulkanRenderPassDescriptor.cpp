/*
-----------------------------------------------------------------------------
This source file is part of OGRE
    (Object-oriented Graphics Rendering Engine)
For the latest info, see http://www.ogre3d.org/

Copyright (c) 2000-present Torus Knot Software Ltd

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

#include "OgreVulkanRenderPassDescriptor.h"

#include "OgreVulkanDevice.h"
#include "OgreVulkanRenderSystem.h"
#include "OgreVulkanTextureGpu.h"
#include "OgreVulkanTextureGpuWindow.h"
#include "OgreVulkanWindow.h"

#include "OgreVulkanMappings.h"
#include "OgreVulkanUtils.h"

#if OGRE_DEBUG_MODE && OGRE_PLATFORM == OGRE_PLATFORM_LINUX
#    include <execinfo.h>  //backtrace
#endif

#define TODO_use_render_pass_that_can_load

namespace Ogre
{
    VulkanRenderPassDescriptor::VulkanRenderPassDescriptor( VulkanQueue *graphicsQueue,
                                                            VulkanRenderSystem *renderSystem ) :
        mSharedFboItor( renderSystem->_getFrameBufferDescMap().end() ),
        mSharedFboFlushItor( renderSystem->_getFlushOnlyDescMap().end() ),
        mTargetWidth( 0u ),
        mTargetHeight( 0u ),
        mQueue( graphicsQueue ),
        mRenderSystem( renderSystem )
    {
    }
    //-----------------------------------------------------------------------------------
    VulkanRenderPassDescriptor::~VulkanRenderPassDescriptor() { releaseFbo(); }
    //-----------------------------------------------------------------------------------
    void VulkanRenderPassDescriptor::checkRenderWindowStatus( void )
    {
        if( ( mNumColourEntries > 0 && mColour[0]->isRenderWindowSpecific() ) ||
            ( mDepth && mDepth->isRenderWindowSpecific() ) )
        {
            if( mNumColourEntries > 1u )
            {
                OGRE_EXCEPT( Exception::ERR_INVALIDPARAMS,
                             "Cannot use RenderWindow as MRT with other colour textures",
                             "VulkanRenderPassDescriptor::colourEntriesModified" );
            }
        }

        calculateSharedKey();
    }
    //-----------------------------------------------------------------------------------
    void VulkanRenderPassDescriptor::calculateSharedKey( void )
    {
        VulkanFrameBufferDescKey key( *this );
        VulkanFrameBufferDescMap &frameBufferDescMap = mRenderSystem->_getFrameBufferDescMap();
        VulkanFrameBufferDescMap::iterator newItor = frameBufferDescMap.find( key );

        if( newItor == frameBufferDescMap.end() )
        {
            VulkanFrameBufferDescValue value;
            value.refCount = 0;
            frameBufferDescMap[key] = value;
            newItor = frameBufferDescMap.find( key );
        }

        ++newItor->second.refCount;

        releaseFbo();

        mSharedFboItor = newItor;
        calculateSharedFlushOnlyKey();
    }
    //-----------------------------------------------------------------------------------
    void VulkanRenderPassDescriptor::calculateSharedFlushOnlyKey( void )
    {
        FrameBufferDescKey key( *this );
        VulkanFlushOnlyDescMap &frameBufferDescMap = mRenderSystem->_getFlushOnlyDescMap();
        VulkanFlushOnlyDescMap::iterator newItor = frameBufferDescMap.find( key );

        if( newItor == frameBufferDescMap.end() )
        {
            VulkanFlushOnlyDescValue value;
            value.refCount = 0;
            frameBufferDescMap[key] = value;
            newItor = frameBufferDescMap.find( key );
        }

        ++newItor->second.refCount;

        mSharedFboFlushItor = newItor;
    }
    //-----------------------------------------------------------------------------------
    VkClearColorValue VulkanRenderPassDescriptor::getClearColour( const ColourValue &clearColour,
                                                                  PixelFormatGpu pixelFormat )
    {
        const bool isInteger = PixelUtil::isInteger( pixelFormat );
        const bool isSigned = false;//PixelUtil::isSigned( pixelFormat );
        VkClearColorValue retVal;
        if( !isInteger )
        {
            for( size_t i = 0u; i < 4u; ++i )
                retVal.float32[i] = static_cast<float>( clearColour[i] );
        }
        else
        {
            if( !isSigned )
            {
                for( size_t i = 0u; i < 4u; ++i )
                    retVal.uint32[i] = static_cast<uint32>( clearColour[i] );
            }
            else
            {
                for( size_t i = 0u; i < 4u; ++i )
                    retVal.int32[i] = static_cast<int32>( clearColour[i] );
            }
        }
        return retVal;
    }
    //-----------------------------------------------------------------------------------
    /**
    @brief VulkanRenderPassDescriptor::setupColourAttachment
        This will setup:
            attachments[currAttachmIdx]
            colourAttachRefs[vkIdx]
            resolveAttachRefs[vkIdx]
            fboDesc.mImageViews[currAttachmIdx]
            fboDesc.mWindowImageViews

        Except mWindowImageViews, all the other variables are *always* written to.
    @param idx [in]
        idx to mColour[idx]
    @param fboDesc [in/out]
    @param attachments [out]
        A pointer to setup VkAttachmentDescription
    @param currAttachmIdx [in/out]
        A value to index attachments[currAttachmIdx]
    @param colourAttachRefs [out]
        A pointer to setup VkAttachmentReference
    @param resolveAttachRefs [out]
        A pointer to setup VkAttachmentReference
    @param vkIdx [in]
        A value to index both colourAttachRefs[vkIdx] & resolveAttachRefs[vkIdx]
        Very often idx == vkIdx except when we skip a colour entry due to being PFG_NULL
    @param resolveTex
        False if we're setting up the main target
        True if we're setting up the resolve target
    */
    void VulkanRenderPassDescriptor::setupColourAttachment(
        const size_t idx, VulkanFrameBufferDescValue &fboDesc, VkAttachmentDescription *attachments,
        uint32 &currAttachmIdx, VkAttachmentReference *colourAttachRefs,
        VkAttachmentReference *resolveAttachRefs, const size_t vkIdx, const bool bResolveTex )
    {
        VulkanTextureGpu* colour = mColour[idx];

        if (!colour->getMsaaTextureName() && bResolveTex)
        {
            // There's no resolve texture to setup
            resolveAttachRefs[vkIdx].attachment = VK_ATTACHMENT_UNUSED;
            resolveAttachRefs[vkIdx].layout = VK_IMAGE_LAYOUT_UNDEFINED;
            return;
        }

        VkImage texName = 0;
        VulkanTextureGpu *texture = colour;

        if( !bResolveTex && texture->getMsaaTextureName())
        {
            texName = texture->getMsaaTextureName();
        }
        else
        {
            texName = texture->getFinalTextureName();
        }

        VkAttachmentDescription &attachment = attachments[currAttachmIdx];
        attachment.format = VulkanMappings::get( texture->getFormat() );
        attachment.samples = bResolveTex ? VK_SAMPLE_COUNT_1_BIT : VkSampleCountFlagBits(colour->getFSAA());
        attachment.loadOp = bResolveTex ? VK_ATTACHMENT_LOAD_OP_DONT_CARE : VK_ATTACHMENT_LOAD_OP_CLEAR;// TODO colour.loadAction );
        attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE; // TODO colour.storeAction
        attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        if( !bResolveTex )
        {
            if (texture->isRenderWindowSpecific() && !texture->isMultisample())
            {
                attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED; // TODO VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                attachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            }
            else
            {
                attachment.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                attachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            }
        }
        else
        {
            attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            if (texture->isRenderWindowSpecific())
                attachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            else
                attachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        }

        const uint8 mipLevel = 0;//bResolveTex ? colour.resolveMipLevel : colour.mipLevel;
        const uint16 slice = 0;//bResolveTex ? colour.resolveSlice : colour.slice;

        if( !texture->isRenderWindowSpecific() || ( texture->isMultisample() && !bResolveTex ) )
        {
            fboDesc.mImageViews[currAttachmIdx] = texture->_createView(mipLevel, 0, slice, 1u, texName);
        }
        else
        {
            fboDesc.mImageViews[currAttachmIdx] = 0;  // Set to null (will be set later, 1 for each FBO)
            auto textureVulkan = dynamic_cast<VulkanTextureGpuWindow*>(texture);

            OGRE_ASSERT_LOW(fboDesc.mWindowImageViews.empty() && "Only one window can be used as target");
            fboDesc.mWindowImageViews = textureVulkan->getWindow()->getSwapchainImageViews();
        }

        if( bResolveTex )
        {
            resolveAttachRefs[vkIdx].attachment = currAttachmIdx;
            resolveAttachRefs[vkIdx].layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            ++currAttachmIdx;
        }
        else
        {
            colourAttachRefs[vkIdx].attachment = currAttachmIdx;
            colourAttachRefs[vkIdx].layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            ++currAttachmIdx;

            // Now repeat with the resolve texture (if applies)
            setupColourAttachment( idx, fboDesc, attachments, currAttachmIdx, colourAttachRefs,
                                   resolveAttachRefs, vkIdx, true );
        }
    }
    //-----------------------------------------------------------------------------------
    VkImageView VulkanRenderPassDescriptor::setupDepthAttachment( VkAttachmentDescription &attachment )
    {
        attachment.format = VulkanMappings::get( mDepth->getFormat() );
        attachment.samples = VkSampleCountFlagBits(std::max(mDepth->getFSAA(), 1u));
        attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        if( 0 )
        {
            attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        }
        else
        {
            attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        }

        if(0) // mDepth.readOnly )
        {
            attachment.initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
            attachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        }
        else
        {
            attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            attachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        }

        VulkanTextureGpu *texture = mDepth;
        VkImage texName =
            texture->getMsaaTextureName() ? texture->getMsaaTextureName() : texture->getFinalTextureName();
        return texture->_createView(0, 0, 0, 1u, texName);
    }
    //-----------------------------------------------------------------------------------
    void VulkanRenderPassDescriptor::setupFbo( VulkanFrameBufferDescValue &fboDesc )
    {
        if( fboDesc.mRenderPass )
            return;  // Already initialized

#if 0
        if( mDepth.texture && mDepth.texture->getResidencyStatus() != GpuResidency::Resident )
        {
            OGRE_EXCEPT( Exception::ERR_INVALIDPARAMS,
                         "RenderTexture '" + mDepth.texture->getName() + "' must be resident!",
                         "VulkanRenderPassDescriptor::updateFbo" );
        }

        if( mStencil.texture && mStencil.texture->getResidencyStatus() != GpuResidency::Resident )
        {
            OGRE_EXCEPT( Exception::ERR_INVALIDPARAMS,
                         "RenderTexture '" + mStencil.texture->getName() + "' must be resident!",
                         "VulkanRenderPassDescriptor::updateFbo" );
        }
#endif
        bool hasRenderWindow = false;

        uint32 attachmentIdx = 0u;
        uint32 numColourAttachments = 0u;
        uint32 windowAttachmentIdx = std::numeric_limits<uint32>::max();
        bool usesResolveAttachments = false;

        // 1 per MRT
        // 1 per MRT MSAA resolve
        // 1 for Depth buffer
        // 1 for Stencil buffer
        VkAttachmentDescription attachments[OGRE_MAX_MULTIPLE_RENDER_TARGETS * 2u + 2u] = {};

        VkAttachmentReference colourAttachRefs[OGRE_MAX_MULTIPLE_RENDER_TARGETS];
        VkAttachmentReference resolveAttachRefs[OGRE_MAX_MULTIPLE_RENDER_TARGETS];
        VkAttachmentReference depthAttachRef;

        for( size_t i = 0; i < mNumColourEntries; ++i )
        {
            hasRenderWindow |= mColour[i]->isRenderWindowSpecific();

            if( mColour[i]->getFormat() == PF_UNKNOWN )
                continue;

            OGRE_ASSERT_HIGH( dynamic_cast<VulkanTextureGpu *>( mColour[i] ) );
            VulkanTextureGpu *textureVulkan = static_cast<VulkanTextureGpu *>( mColour[i] );

            if( textureVulkan->isRenderWindowSpecific() )
            {
                windowAttachmentIdx = attachmentIdx;
                // use the resolve texture idx
                if (textureVulkan->getMsaaTextureName())
                    windowAttachmentIdx++;
            }

            setupColourAttachment( i, fboDesc, attachments, attachmentIdx, colourAttachRefs,
                                   resolveAttachRefs, numColourAttachments, false );
            if( resolveAttachRefs[numColourAttachments].attachment != VK_ATTACHMENT_UNUSED )
                usesResolveAttachments = true;
            ++numColourAttachments;
        }

        if( mDepth )
        {
            fboDesc.mImageViews[attachmentIdx] = setupDepthAttachment( attachments[attachmentIdx] );
            depthAttachRef.attachment = attachmentIdx;
            if(0)// mDepth.readOnly )
                depthAttachRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
            else
                depthAttachRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            ++attachmentIdx;
        }

        VkSubpassDescription subpass = {VK_PIPELINE_BIND_POINT_GRAPHICS};
        subpass.inputAttachmentCount = 0u;
        subpass.colorAttachmentCount = numColourAttachments;
        subpass.pColorAttachments = colourAttachRefs;
        subpass.pResolveAttachments = usesResolveAttachments ? resolveAttachRefs : 0;
        subpass.pDepthStencilAttachment = mDepth ? &depthAttachRef : 0;

        fboDesc.mNumImageViews = attachmentIdx;

        VkRenderPassCreateInfo renderPassCreateInfo = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        renderPassCreateInfo.attachmentCount = attachmentIdx;
        renderPassCreateInfo.pAttachments = attachments;
        renderPassCreateInfo.subpassCount = 1u;
        renderPassCreateInfo.pSubpasses = &subpass;
        OGRE_VK_CHECK(vkCreateRenderPass( mQueue->mDevice, &renderPassCreateInfo, 0, &fboDesc.mRenderPass ));

        VkFramebufferCreateInfo fbCreateInfo = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fbCreateInfo.renderPass = fboDesc.mRenderPass;
        fbCreateInfo.attachmentCount = attachmentIdx;
        fbCreateInfo.pAttachments = fboDesc.mImageViews;
        fbCreateInfo.width = mTargetWidth;
        fbCreateInfo.height = mTargetHeight;
        fbCreateInfo.layers = 1u;

        const size_t numFramebuffers = std::max<size_t>( fboDesc.mWindowImageViews.size(), 1u );
        fboDesc.mFramebuffers.resize( numFramebuffers );
        for( size_t i = 0u; i < numFramebuffers; ++i )
        {
            if( !fboDesc.mWindowImageViews.empty() )
                fboDesc.mImageViews[windowAttachmentIdx] = fboDesc.mWindowImageViews[i];
            OGRE_VK_CHECK(vkCreateFramebuffer(mQueue->mDevice, &fbCreateInfo, 0, &fboDesc.mFramebuffers[i]));
            if( !fboDesc.mWindowImageViews.empty() )
                fboDesc.mImageViews[windowAttachmentIdx] = 0;
        }
    }
    //-----------------------------------------------------------------------------------
    void VulkanRenderPassDescriptor::releaseFbo( void )
    {
        {
            VulkanFrameBufferDescMap &frameBufferDescMap = mRenderSystem->_getFrameBufferDescMap();
            if( mSharedFboItor != frameBufferDescMap.end() )
            {
                --mSharedFboItor->second.refCount;
                if( !mSharedFboItor->second.refCount )
                {
                    destroyFbo( mQueue, mSharedFboItor->second );
                    frameBufferDescMap.erase( mSharedFboItor );
                }
                mSharedFboItor = frameBufferDescMap.end();
            }
        }
        {
            VulkanFlushOnlyDescMap &frameBufferDescMap = mRenderSystem->_getFlushOnlyDescMap();
            if( mSharedFboFlushItor != frameBufferDescMap.end() )
            {
                --mSharedFboFlushItor->second.refCount;
                if( !mSharedFboFlushItor->second.refCount )
                    frameBufferDescMap.erase( mSharedFboFlushItor );
                mSharedFboFlushItor = frameBufferDescMap.end();
            }
        }
    }
    //-----------------------------------------------------------------------------------
    void VulkanRenderPassDescriptor::destroyFbo( VulkanQueue *queue,
                                                 VulkanFrameBufferDescValue &fboDesc )
    {
        //VaoManager *vaoManager = queue->getVaoManager();

        {
            FastArray<VkFramebuffer>::const_iterator itor = fboDesc.mFramebuffers.begin();
            FastArray<VkFramebuffer>::const_iterator endt = fboDesc.mFramebuffers.end();
            while( itor != endt )
                vkDestroyFramebuffer( queue->mDevice, *itor++, 0 );
            fboDesc.mFramebuffers.clear();
        }

        for( size_t i = 0u; i < fboDesc.mNumImageViews; ++i )
        {
            if( fboDesc.mImageViews[i] )
            {
                vkDestroyImageView( queue->mDevice, fboDesc.mImageViews[i], 0 );
                fboDesc.mImageViews[i] = 0;
            }
        }
        fboDesc.mNumImageViews = 0u;

        vkDestroyRenderPass( queue->mDevice, fboDesc.mRenderPass, 0 );
        fboDesc.mRenderPass = 0;
    }
    //-----------------------------------------------------------------------------------
    void VulkanRenderPassDescriptor::notifySwapchainCreated( VulkanWindow *window )
    {
        mNumColourEntries = 1;
        mColour[0] = window->getTexture(); // FIXME
        mDepth = window->getDepthTexture();
        //if( mNumColourEntries > 0 && mColour[0].texture->isRenderWindowSpecific() &&
        //    mColour[0].texture == window->getTexture() )
        {
            entriesModified( RenderPassDescriptor::All );
        }
    }
    //-----------------------------------------------------------------------------------
    void VulkanRenderPassDescriptor::notifySwapchainDestroyed( VulkanWindow *window )
    {
        if (mNumColourEntries > 0 && mColour[0]->isRenderWindowSpecific() && mColour[0] == window->getTexture())
        {
            releaseFbo();
        }
    }
    //-----------------------------------------------------------------------------------
    void VulkanRenderPassDescriptor::entriesModified( uint32 entryTypes )
    {
        //RenderPassDescriptor::entriesModified( entryTypes );

        checkRenderWindowStatus();

        TextureGpu *anyTargetTexture = 0;
        const uint8 numColourEntries = mNumColourEntries;
        for( int i = 0; i < numColourEntries && !anyTargetTexture; ++i )
            anyTargetTexture = mColour[i];
        if( !anyTargetTexture )
            anyTargetTexture = mDepth;

        mTargetWidth = 0u;
        mTargetHeight = 0u;
        if( anyTargetTexture )
        {
            mTargetWidth = anyTargetTexture->getWidth();
            mTargetHeight = anyTargetTexture->getHeight();
        }

        if( entryTypes & RenderPassDescriptor::All )
            setupFbo( mSharedFboItor->second );
    }
    //-----------------------------------------------------------------------------------
    void VulkanRenderPassDescriptor::setClearColour( uint8 idx, const ColourValue &clearColour )
    {
        //RenderPassDescriptor::setClearColour( idx, clearColour );

        size_t attachmentIdx = 0u;
        for( size_t i = 0u; i < idx; ++i )
        {
            ++attachmentIdx;
            if (mColour[i]->getMsaaTextureName())
                ++attachmentIdx;
        }

        mClearValues[attachmentIdx].color =
            getClearColour( clearColour, mColour[idx]->getFormat() );
    }
    //-----------------------------------------------------------------------------------
    void VulkanRenderPassDescriptor::setClearDepth( Real clearDepth )
    {
        //RenderPassDescriptor::setClearDepth( clearDepth );
        if( mDepth && mSharedFboItor != mRenderSystem->_getFrameBufferDescMap().end() )
        {
            size_t attachmentIdx = mSharedFboItor->second.mNumImageViews - 1u;
            if( !mRenderSystem->isReverseDepthBufferEnabled() )
                mClearValues[attachmentIdx].depthStencil.depth = static_cast<float>(clearDepth);
            else
            {
                mClearValues[attachmentIdx].depthStencil.depth = static_cast<float>(Real(1.0) - clearDepth);
            }
        }
    }
    //-----------------------------------------------------------------------------------
    void VulkanRenderPassDescriptor::setClearStencil( uint32 clearStencil )
    {
        //RenderPassDescriptor::setClearStencil( clearStencil );
        if (mDepth && mSharedFboItor != mRenderSystem->_getFrameBufferDescMap().end())
        {
            size_t attachmentIdx = mSharedFboItor->second.mNumImageViews - 1u;
            mClearValues[attachmentIdx].depthStencil.stencil = clearStencil;
        }
    }
    //-----------------------------------------------------------------------------------
    void VulkanRenderPassDescriptor::setClearColour( const ColourValue &clearColour )
    {
        const size_t numColourEntries = mNumColourEntries;
        size_t attachmentIdx = 0u;
        for( size_t i = 0u; i < numColourEntries; ++i )
        {
            mClearValues[attachmentIdx].color = getClearColour(clearColour, mColour[i]->getFormat());
            ++attachmentIdx;
            if (mColour[i]->getMsaaTextureName())
                ++attachmentIdx;
        }
    }
    //-----------------------------------------------------------------------------------
    uint32 VulkanRenderPassDescriptor::willSwitchTo( VulkanRenderPassDescriptor *newDesc,
                                                     bool warnIfRtvWasFlushed ) const
    {
        uint32 entriesToFlush = 0;

        if( !newDesc ||                                                   //
            this->mSharedFboFlushItor != newDesc->mSharedFboFlushItor ||  //
            this->mInformationOnly || newDesc->mInformationOnly )
        {
            entriesToFlush = RenderPassDescriptor::All;
        }

        if( warnIfRtvWasFlushed )
            newDesc->checkWarnIfRtvWasFlushed( entriesToFlush );

        return entriesToFlush;
    }
    //-----------------------------------------------------------------------------------
    void VulkanRenderPassDescriptor::performLoadActions()
    {
        if( mInformationOnly )
            return;

        VkCommandBuffer cmdBuffer = mQueue->mCurrentCmdBuffer;

        const VulkanFrameBufferDescValue &fboDesc = mSharedFboItor->second;

        size_t fboIdx = 0u;
        if( !fboDesc.mWindowImageViews.empty() )
        {
            VulkanTextureGpuWindow* textureVulkan = static_cast<VulkanTextureGpuWindow*>(mColour[0]);
            fboIdx = textureVulkan->getCurrentImageIdx();

            VkSemaphore semaphore = textureVulkan->getImageAcquiredSemaphore();
            if( semaphore )
            {
                // We cannot start executing color attachment commands until the semaphore says so
                mQueue->addWindowToWaitFor( semaphore );
            }
        }

        VkRenderPassBeginInfo passBeginInfo = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        passBeginInfo.renderPass = fboDesc.mRenderPass;
        passBeginInfo.framebuffer = fboDesc.mFramebuffers[fboIdx];
        passBeginInfo.renderArea.offset.x = 0;
        passBeginInfo.renderArea.offset.y = 0;
        passBeginInfo.renderArea.extent.width = mTargetWidth;
        passBeginInfo.renderArea.extent.height = mTargetHeight;
        passBeginInfo.clearValueCount = sizeof( mClearValues ) / sizeof( mClearValues[0] );
        passBeginInfo.pClearValues = mClearValues;

        vkCmdBeginRenderPass( cmdBuffer, &passBeginInfo, VK_SUBPASS_CONTENTS_INLINE );
    }
    //-----------------------------------------------------------------------------------
    void VulkanRenderPassDescriptor::performStoreActions()
    {
        if( mInformationOnly )
            return;

        if( mQueue->getEncoderState() != VulkanQueue::EncoderGraphicsOpen )
            return;

        vkCmdEndRenderPass( mQueue->mCurrentCmdBuffer );

        // End (if exists) the render command encoder tied to this RenderPassDesc.
        // Another encoder will have to be created, and don't let ours linger
        // since mCurrentRenderPassDescriptor probably doesn't even point to 'this'
        mQueue->endAllEncoders( false );
    }
    //-----------------------------------------------------------------------------------
    //-----------------------------------------------------------------------------------
    //-----------------------------------------------------------------------------------
    VulkanFrameBufferDescKey::VulkanFrameBufferDescKey() {}
    //-----------------------------------------------------------------------------------
    VulkanFrameBufferDescKey::VulkanFrameBufferDescKey( const RenderPassDescriptor &desc )
    {
    }
    //-----------------------------------------------------------------------------------
    bool VulkanFrameBufferDescKey::operator<( const VulkanFrameBufferDescKey &other ) const
    {
        if( this->numColourEntries != other.numColourEntries )
            return this->numColourEntries < other.numColourEntries;

        return false;
    }
    //-----------------------------------------------------------------------------------
    //-----------------------------------------------------------------------------------
    //-----------------------------------------------------------------------------------
    VulkanFlushOnlyDescValue::VulkanFlushOnlyDescValue() : refCount( 0 ) {}
    //-----------------------------------------------------------------------------------
    VulkanFrameBufferDescValue::VulkanFrameBufferDescValue() :
        refCount( 0u ),
        mNumImageViews( 0u ),
        mRenderPass( 0 )
    {
        memset( mImageViews, 0, sizeof( mImageViews ) );
    }
}  // namespace Ogre