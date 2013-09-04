/* -*-c++-*- */
/* osgEarth - Dynamic map generation toolkit for OpenSceneGraph
* Copyright 2008-2013 Pelican Mapping
* http://osgearth.org
*
* osgEarth is free software; you can redistribute it and/or modify
* it under the terms of the GNU Lesser General Public License as published by
* the Free Software Foundation; either version 2 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU Lesser General Public License for more details.
*
* You should have received a copy of the GNU Lesser General Public License
* along with this program.  If not, see <http://www.gnu.org/licenses/>
*/
#include "TilePagedLOD"
#include "TileNodeRegistry"
#include <osg/Version>

using namespace osgEarth_engine_mp;
using namespace osgEarth;

#define LC "[TilePagedLOD] "

//#define OE_TEST OE_INFO
#define OE_TEST OE_NULL


TilePagedLOD::TilePagedLOD(TileGroup*        tilegroup,
                           const TileKey&    subkey,
                           const UID&        engineUID,
                           TileNodeRegistry* live,
                           TileNodeRegistry* dead) :
osg::PagedLOD(),
_tilegroup ( tilegroup ),
_live      ( live ),
_dead      ( dead ),
_upsampling( false ),
_container ( 0L )
{
    _numChildrenThatCannotBeExpired = 0;

    // set up the paging properties:
    _prefix = Stringify() << subkey.str() << "." << engineUID << ".";
    this->setRange   ( 0, 0.0f, FLT_MAX );
    this->setFileName( 0, Stringify() << _prefix << ".osgearth_engine_mp_tile" );
}


// The osgDB::DatabasePager will call this method when merging a new child
// into the scene graph.
bool
TilePagedLOD::addChild(osg::Node* node)
{
    // First check whether this is a new TileGroup (a group that contains a TileNode
    // and children paged LODs). If so, add it normally and inform our parent.
    TileGroup* subtilegroup = dynamic_cast<TileGroup*>(node);
    if ( subtilegroup )
    {
        _container = subtilegroup;
        _live->add( subtilegroup->getTileNode() );
        ++_tilegroup->numSubtilesLoaded();
        return osg::PagedLOD::addChild( node );
    }

    // If that fails, check whether this is a simple TileNode. This means that 
    // this is a leaf node in the graph (no children), or it's a replacement
    // tile for our existing tileNode, or possibly that it has no data at all 
    // (and we need to create an upsampled child to complete the required set of
    // four).
    TileNode* subtile = dynamic_cast<TileNode*>(node);
    if ( subtile )
    {
        // If it's a legit tile, add it normally and inform our parent.
        if ( subtile->isValid() )
        {
            if ( _container == 0L )
            {
                // The initial valid tile node we've been waiting for. Insert it.
                _container = subtile;
                _upsampling = false;
                _live->add( subtile );
                ++_tilegroup->numSubtilesLoaded();
                return osg::PagedLOD::addChild( node );
            }
            else
            {
                // A replacement tile. Replace the tile node we have with this
                // new version and update the registry.
                _upsampling = false;

                TileNode* old_subtile = _container->getTileNode();

                if ( dynamic_cast<TileNode*>(_container) )
                {
                    // it's a TileNode, replace it here.
                    this->setChild(0, subtile);
                    _container = subtile;
                }
                else
                {
                    // otherwise replace it in its parent tile group. If the new subtile has
                    // a cull callback, remove it because the container already has one.
                    subtile->setCullCallback( 0L );
                    static_cast<TileGroup*>(_container)->setTileNode( subtile );
                    //OE_WARN << LC << "Replaced tile " << _container->getTileNode()->getKey().str() << std::endl;
                }

                // remove the tile-replacement filename.
                _rangeList.resize( 1 );
                _perRangeDataList.resize( 1 );

                // update the registry:
                _live->remove( old_subtile );
                _live->add( subtile );

                return true;
            }
        }

        // if it's an "invalid" marker tile, queue up a request to create an upsampled
        // version of the parent to stick in its place.
        else
        {
            if ( !_upsampling )
            {
                OE_DEBUG << LC << "Try to upsample " << _prefix << std::endl;
                _upsampling = true;
                ++_tilegroup->numSubtilesUpsampling();
                unsigned index = this->getNumFileNames() - 1;
                this->setFileName( index, Stringify() << _prefix << ".osgearth_engine_mp_upsampled_tile" );
            }
            else
            {
                // Getting here means that the upsampling request failed, in which case
                // it will not be possible to complete the set of four; so we just have
                // to cancel this entire LOD.
                _tilegroup->cancelSubtiles();
            }
            return false;
        }
    }
    
    // Getting here means there's an internal error -- the addChild data was
    // of an unexpected node type. This should never happen.
    OE_WARN << LC << "TilePagedLOD fail." << std::endl;
    if ( !node )
        OE_WARN << LC << ".... node is NULL" << std::endl;
    else
        OE_WARN << LC << "... node is a " << node->className() << std::endl;

    return false;
}


void
TilePagedLOD::traverse(osg::NodeVisitor& nv)
{
    // Only traverse the TileNode if our neighbors (the other members of
    // our group of four) are ready as well.
    if ( _children.size() > 0 )
    {
        bool ready = _tilegroup->numSubtilesLoaded() >= 4;
        _children[0]->setNodeMask(ready? ~0 : 0);

        // Check whether the TileNode is marked dirty. If so, install a new pager request 
        // to reload and replace the TileNode.
        if ( this->getNumFileNames() < 2 && _container->getTileNode()->isReadyForUpdate() && nv.getVisitorType() == nv.CULL_VISITOR )
        {
            Threading::ScopedMutexLock exclusive( _updateMutex );

            if ( this->getNumFileNames() < 2 ) // double-check pattern
            {
                //OE_WARN << LC << "Queuing request for replacement: " << _container->getTileNode()->getKey().str() << std::endl;
                this->setFileName( 1, Stringify() << _prefix << ".osgearth_engine_mp_standalone_tile" );
                this->setRange( 1, 0, FLT_MAX );
            }
        }
    }

    osg::PagedLOD::traverse( nv );
}


// The osgDB::DatabasePager will call this automatically to purge expired
// tiles from the scene grpah.
bool
TilePagedLOD::removeExpiredChildren(double         expiryTime, 
                                    unsigned       expiryFrame, 
                                    osg::NodeList& removedChildren)
{
    if (_children.size()>_numChildrenThatCannotBeExpired)
    {
        unsigned cindex = _children.size() - 1;

        double   minExpiryTime   = 0.0;
        unsigned minExpiryFrames = 0;

        // these were added in osg 3.1.0+
#if OSG_VERSION_GREATER_OR_EQUAL(3,1,0)
        minExpiryTime   = _perRangeDataList[cindex]._minExpiryTime;
        minExpiryFrames = _perRangeDataList[cindex]._minExpiryFrames;
#endif

        if (!_perRangeDataList[cindex]._filename.empty() &&
            _perRangeDataList[cindex]._timeStamp   + minExpiryTime   < expiryTime &&
            _perRangeDataList[cindex]._frameNumber + minExpiryFrames < expiryFrame)
        {
            osg::Node* nodeToRemove = _children[cindex].get();
            removedChildren.push_back(nodeToRemove);

            TileNode* tilenode = dynamic_cast<TileNode*>(nodeToRemove);
            if (!tilenode)
                tilenode = dynamic_cast<TileGroup*>(nodeToRemove)->getTileNode();
            if ( tilenode )
            {
                if ( _live )
                    _live->remove( tilenode );
                if ( _dead )
                    _dead->add( tilenode );
            }

            OE_DEBUG << "Expired " << _prefix << std::endl;
            --_tilegroup->numSubtilesLoaded();

            return Group::removeChildren(cindex,1);
        }
    }
    return false;
}
