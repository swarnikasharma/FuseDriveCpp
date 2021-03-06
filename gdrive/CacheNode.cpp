/* 
 * File:   CacheNode.cpp
 * Author: me
 * 
 * Created on October 19, 2015, 11:11 AM
 */



#include "CacheNode.hpp"
#include "Cache.hpp"
#include "Gdrive.hpp"
#include "GdriveFile.hpp"
#include "Util.hpp"

#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <assert.h>
#include <sys/stat.h>
#include <sstream>


using namespace std;
namespace fusedrive
{
    CacheNode** gdrive_cache_get_head_ptr(Cache& cache)
    {
        return &cache.mpCacheHead;
    }

    CacheNode* CacheNode::retrieveNode(Cache& cache, CacheNode* pParent, 
            CacheNode** ppNode, const string& fileId, bool addIfDoesntExist, 
            bool& alreadyExists)
    {
        Gdrive& gInfo = cache.getGdrive();
        
        alreadyExists = false;

        if (*ppNode == NULL)
        {
            // Item doesn't exist in the cache. Either fail, or create a new item.
            if (!addIfDoesntExist)
            {
                // Not allowed to create a new item, return failure.
                return NULL;
            }
            // else create a new item.
            *ppNode = pParent ? new CacheNode(pParent) : new CacheNode(gInfo);
            
            // Convenience to avoid things like "return &((*ppNode)->fileinfo);"
            CacheNode* pNode = *ppNode;

            // Get the fileinfo
            stringstream url;
            url << Gdrive::GDRIVE_URL_FILES <<  "/" << fileId;
            HttpTransfer xfer(gInfo);
            
            xfer.setUrl(url.str())
                .setRequestType(HttpTransfer::GET);
            
            if (xfer.execute() != 0)
            {
                return NULL;
            }
            
            if (xfer.getHttpResponse() >= 400)
            {
                // Download or request error
                return NULL;
            }
            Json jsonObj(xfer.getData());
            if (!jsonObj.isValid())
            {
                // Couldn't convert network response to JSON
                return NULL;
            }
            pNode->updateFromJson(jsonObj);

            return pNode;
        }

        // Convenience to avoid things like "&((*ppNode)->pRight)"
        CacheNode* pNode = *ppNode;

        // Root node exists, try to find the fileId in the tree.
        int cmp = fileId.compare(pNode->fileinfo.id);
        if (cmp == 0)
        {
            // Found it at the current node.
            alreadyExists = true;
            return pNode;
        }
        else if (cmp < 0)
        {
            // fileId is less than the current node. Look for it on the left.
            return retrieveNode(cache, pNode, &(pNode->pLeft), fileId, 
                    addIfDoesntExist, alreadyExists);
        }
        else
        {
            // fileId is greater than the current node. Look for it on the right.
            return retrieveNode(cache, pNode, &(pNode->pRight), fileId, 
                    addIfDoesntExist, alreadyExists);
        }
    }

    CacheNode* CacheNode::retrieveNode(Cache& cache, CacheNode* pParent, 
            CacheNode** ppNode, const string& fileId, bool addIfDoesntExist)
    {
        bool dummy = false;
        return retrieveNode(cache, pParent, ppNode, fileId, 
                addIfDoesntExist, dummy);
    }
    
    Gdrive& CacheNode::getGdrive() const
    {
        return gInfo;
    }
    
    void CacheNode::deleteNode()
    {
        // The address of the pointer aimed at this node. If this is the root node,
        // then it will be a pointer passed in from outside. Otherwise, it is the
        // pLeft or pRight member of the parent.
        CacheNode** ppFromParent;
        if (pParent == NULL)
        {
            // This is the root. Take the pointer that was passed in.
            ppFromParent = gdrive_cache_get_head_ptr(mCache);
        }
        else
        {
            // Not the root. Find whether the node hangs from the left or right side
            // of its parent.
            assert(pParent->pLeft == this || 
                    pParent->pRight == this);
            ppFromParent = (pParent->pLeft == this) ? 
                &pParent->pLeft : &pParent->pRight;

        }

        // Simplest special case. pNode has no descendents.  Just delete it, and
        // set the pointer from the parent to NULL.
        if (pLeft == NULL && pRight == NULL)
        {
            *ppFromParent = NULL;
            delete this;
            return;
        }

        // Second special case. pNode has one side empty. Promote the descendent on
        // the other side into pNode's place.
        if (pLeft == NULL)
        {
            *ppFromParent = pRight;
            pRight->pParent = pParent;
            delete this;
            return;
        }
        if (pRight == NULL)
        {
            *ppFromParent = pLeft;
            pLeft->pParent = pParent;
            delete this;
            return;
        }

        // General case with descendents on both sides. Find the node with the 
        // closest value to pNode in one of its subtrees (leftmost node of the right
        // subtree, or rightmost node of the left subtree), and switch places with
        // pNode.  Which side we use doesn't really matter.  We'll rather 
        // arbitrarily decide to use the same side subtree as the side from which
        // pNode hangs off its parent (if pNode is on the right side of its parent,
        // find the leftmost node of the right subtree), and treat the case where
        // pNode is the root the same as if it were on the left side of its parent.
        CacheNode* pSwap = NULL;
        CacheNode** ppToSwap = NULL;
        if (pParent != NULL && pParent->pRight == this)
        {
            // Find the leftmost node of the right subtree.
            pSwap = pRight;
            ppToSwap = &pRight;
            while (pSwap->pLeft != NULL)
            {
                ppToSwap = &pSwap->pLeft;
                pSwap = pSwap->pLeft;
            }
        }
        else
        {
            // Find the rightmost node of the left subtree.
            pSwap = pLeft;
            ppToSwap = &pLeft;
            while (pSwap->pRight != NULL)
            {
                ppToSwap = &pSwap->pRight;
                pSwap = pSwap->pRight;
            }
        }

        // Swap the nodes
        SwapNodes(ppFromParent, this, ppToSwap, pSwap);

        // Now delete the node from its new position.
        deleteNode();
    }

    void CacheNode::markDeleted()
    {
        deleted = true;
        if (openCount == 0)
        {
            deleteNode();
        }
    }

    bool CacheNode::isDeleted() const
{
    return deleted;
}
    
    void CacheNode::freeAll()
    {
        // Free all the descendents first.
        if (pLeft)
        {
            pLeft->freeAll();
        }
        if (pRight)
        {
            pRight->freeAll();
        }

        delete this;
    }

    time_t CacheNode::getUpdateTime() const
    {
        return lastUpdateTime;
    }

    enum Gdrive_Filetype CacheNode::getFiletype() const
    {
        return fileinfo.type;
    }

    Fileinfo& CacheNode::getFileinfo()
    {
        return fileinfo;
    }


    void CacheNode::updateFromJson(Json& jsonObj)
    {
        fileinfo.clear();
        fileinfo.readJson(jsonObj);

        // Mark the node as having been updated.
        lastUpdateTime = time(NULL);
    }

    void CacheNode::deleteFileContents(FileContents& contentsToDelete)
    {
        delete &contentsToDelete;
    }


    bool CacheNode::isDirty() const
    {
        return dirty;
    }
    
    void CacheNode::setDirty(bool val)
    {
        dirty = val;
    }
    
    int CacheNode::getOpenCount() const
    {
        return openCount;
    }
        
    int CacheNode::getOpenWriteCount() const
    {
        return openWrites;
    }

    void CacheNode::incrementOpenCount(bool isWrite)
    {
        openCount++;
        if (isWrite)
        {
            openWrites++;
        }
    }

    void CacheNode::decrementOpenCount(bool isWrite)
    {
        openCount--;
        if (isWrite)
        {
            openWrites--;
        }
    }
    
    bool CacheNode::checkPermissions(int accessFlags) const
    {
        // What permissions do we have?
        unsigned int perms = fileinfo.getRealPermissions();

        // What permissions do we need?
        unsigned int neededPerms = 0;
        // At least on my system, O_RDONLY is 0, which prevents testing for the
        // individual bit flag. On systems like mine, just assume we always need
        // read access. If there are other systems that have a different O_RDONLY
        // value, we'll test for the flag on those systems.
        if ((O_RDONLY == 0) || (accessFlags & O_RDONLY) || (accessFlags & O_RDWR))
        {
            neededPerms = neededPerms | S_IROTH;
        }
        if ((accessFlags & O_WRONLY) || (accessFlags & O_RDWR))
        {
            neededPerms = neededPerms | S_IWOTH;
        }

        // If there is anything we need but don't have, return false.
        return !(neededPerms & ~perms);
    }

    void CacheNode::clearContents()
    {
        if (pContents)
        {
            pContents->freeAll();
        }
        pContents = NULL;
    }
    
    bool CacheNode::hasContents() const
    {
        return (pContents != NULL);
    }
        
    FileContents* CacheNode::findChunk(off_t offset) const
    {
        if (!pContents)
        {
            // No contents to search, return failure
            return NULL;
        }
        
        return pContents->findChunk(offset);
    }
    
    FileContents** CacheNode::getContentsListPtr()
    {
        return &pContents;
    }
    
    void CacheNode::deleteContentsAfterOffset(off_t offset)
    {
        if (pContents)
        {
            pContents->deleteAfterOffset(offset);
        }
    }
    
    CacheNode::CacheNode(CacheNode* pParent)
    : gInfo(pParent->gInfo), fileinfo(pParent->gInfo), 
            mCache(pParent->gInfo.getCache()), pParent(pParent)
    {
        init();
    }
        
    CacheNode::CacheNode(Gdrive& gInfo)
    : gInfo(gInfo), fileinfo(gInfo), mCache(gInfo.getCache()), 
            pParent(NULL)
    {
        init();
    }
        
    void CacheNode::init()
    {
        lastUpdateTime = 0;
        openCount = 0;
        openWrites = 0;
        dirty = false;
        deleted = false;
        pContents = NULL;
        pLeft = NULL;
        pRight = NULL;
    }

    void CacheNode::SwapNodes(CacheNode** ppFromParentOne, 
                                  CacheNode* pNodeOne, 
                                  CacheNode** ppFromParentTwo, 
                                  CacheNode* pNodeTwo)
    {
        // Make sure pNodeOne is not a descendent of pNodeTwo
        CacheNode* pParent = pNodeOne->pParent;
        while (pParent != NULL)
        {
            if (pParent == pNodeTwo)
            {
                // Reverse the order of the parameters
                SwapNodes(ppFromParentTwo, pNodeTwo, ppFromParentOne, 
                        pNodeOne);
                return;
            }
            pParent = pParent->pParent;
        }

        CacheNode* pTempParent = pNodeOne->pParent;
        CacheNode* pTempLeft = pNodeOne->pLeft;
        CacheNode* pTempRight = pNodeOne->pRight;

        if (pNodeOne->pLeft == pNodeTwo || pNodeOne->pRight == pNodeTwo)
        {
            // Node Two is a direct child of Node One

            pNodeOne->pLeft = pNodeTwo->pLeft;
            pNodeOne->pRight = pNodeTwo->pRight;

            if (pTempLeft == pNodeTwo)
            {
                pNodeTwo->pLeft = pNodeOne;
                pNodeTwo->pRight = pTempRight;
            }
            else
            {
                pNodeTwo->pLeft = pTempLeft;
                pNodeTwo->pRight = pNodeOne;
            }

            // Don't touch *ppFromParentTwo - it's either pNodeOne->pLeft 
            // or pNodeOne->pRight.
        }
        else
        {
            // Not direct parent/child

            pNodeOne->pParent = pNodeTwo->pParent;
            pNodeOne->pLeft = pNodeTwo->pLeft;
            pNodeOne->pRight = pNodeTwo->pRight;

            pNodeTwo->pLeft = pTempLeft;
            pNodeTwo->pRight = pTempRight;

            *ppFromParentTwo = pNodeOne;
        }

        pNodeTwo->pParent = pTempParent;
        *ppFromParentOne = pNodeTwo;

        // Fix the pParent pointers in each of the descendents. If pNodeTwo was
        // originally a direct child of pNodeOne, this also updates pNodeOne's
        // pParent.
        if (pNodeOne->pLeft)
        {
            pNodeOne->pLeft->pParent = pNodeOne;
        }
        if (pNodeOne->pRight)
        {
            pNodeOne->pRight->pParent = pNodeOne;
        }
        if (pNodeTwo->pLeft)
        {
            pNodeTwo->pLeft->pParent = pNodeTwo;
        }
        if (pNodeTwo->pRight)
        {
            pNodeTwo->pRight->pParent = pNodeTwo; 
        }
    }


    FileContents& CacheNode::addContents()
    {
        // Create the actual Gdrive_File_Contents struct, and add it to the existing
        // chain if there is one.
        FileContents& pNewContents = FileContents::addNewChunk(*this);
        
        // If there is no existing chain, point to the new struct as the start of a
        // new chain.
        if (pContents == NULL)
        {
            pContents = &pNewContents;
        }


        return pNewContents;
    }

    FileContents* CacheNode::createChunk(off_t offset, 
        size_t size, bool fillChunk)
    {
        // Get the normal chunk size for this file, the smallest multiple of
        // minChunkSize that results in maxChunks or fewer chunks. Avoid creating
        // a chunk of size 0 by forcing fileSize to be at least 1.
        size_t fileSize = (fileinfo.size > 0) ? fileinfo.size : 1;
        int maxChunks = gInfo.getMaxChunks();
        size_t minChunkSize = gInfo.getMinChunkSize();

        size_t perfectChunkSize = Util::divideCeil(fileSize, maxChunks);
        size_t chunkSize = Util::divideCeil(perfectChunkSize, minChunkSize) * 
                minChunkSize;

        // The actual chunk may be a multiple of chunkSize.  A read that starts at
        // "offset" and is "size" bytes long should be within this single chunk.
        off_t chunkStart = (offset / chunkSize) * chunkSize;
        off_t chunkOffset = offset % chunkSize;
        off_t endChunkOffset = chunkOffset + size - 1;
        size_t realChunkSize = Util::divideCeil(endChunkOffset, chunkSize) * 
                chunkSize;

        FileContents& pContents = addContents();
        
        if (fillChunk)
        {
            int success = pContents.fillChunk(fileinfo.id, 
                    chunkStart, realChunkSize);
            if (success != 0)
            {
                // Didn't write the file.  Clean up the new Gdrive_File_Contents 
                // struct
                deleteFileContents(pContents);
                return NULL;
            }
        }
        // else we're not filling the chunk, do nothing

        // Success
        return &pContents;
    }

    CacheNode::~CacheNode()
    {
        clearContents();
    }

}
