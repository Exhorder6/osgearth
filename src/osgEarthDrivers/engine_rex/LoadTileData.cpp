/* -*-c++-*- */
/* osgEarth - Dynamic map generation toolkit for OpenSceneGraph
* Copyright 2008-2014 Pelican Mapping
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
#include "LoadTileData"
#include "MPTexture"
#include <osgEarth/TerrainEngineNode>
#include <osg/NodeVisitor>

using namespace osgEarth::Drivers::RexTerrainEngine;
using namespace osgEarth;

#define LC "[LoadTileData] "

namespace
{
    // Visitor that recalculates the sampler inheritance matrices in a graph.
    struct UpdateInheritance : public osg::NodeVisitor
    {
        UpdateInheritance(const RenderBindings& bindings, const SelectionInfo& selectionInfo, const MapInfo& mapInfo, unsigned tileSize)
            : _bindings(bindings)
            , _selectionInfo(selectionInfo)
            , _mapInfo(mapInfo)
            , _tileSize(tileSize)
        {
            setTraversalMode( TRAVERSE_ALL_CHILDREN );
        }

        void apply(osg::Group& node)
        {
            TileNode* tilenode = dynamic_cast<TileNode*>(&node);
            if ( tilenode )
            {
                tilenode->inheritState( tilenode->getParentTile(), _bindings, _selectionInfo );
                tilenode->updateElevationData( _bindings, _mapInfo, _tileSize );
            }

            traverse(node);
        }

        const RenderBindings& _bindings;
        const SelectionInfo&  _selectionInfo;
        const MapInfo&        _mapInfo;
        const unsigned        _tileSize;
    };
}

//............................................................................


LoadTileData::LoadTileData(TileNode* tilenode, EngineContext* context) :
_tilenode(tilenode),
_context(context)
{
    //nop
}


void
LoadTileData::invoke()
{
    osg::ref_ptr<TileNode> tilenode;
    if ( _tilenode.lock(tilenode) )
    {
        _model = _context->getEngine()->createTileModel(
            _context->getMapFrame(),
            tilenode->getTileKey(),
            0L ); // progress

        // Prep the stateset for merging (and for GL pre-compile).
        if ( _model.valid() )
        {
            const RenderBindings& bindings = _context->getRenderBindings();

            osg::StateSet* stateSet = getStateSet();

            if ( _model->colorLayers().size() > 0 )
            {
                const SamplerBinding* colorBinding = SamplerBinding::findUsage(bindings, SamplerBinding::COLOR);
                if ( colorBinding )
                {
                    osg::ref_ptr<MPTexture> mptex = new MPTexture();

                    for(TerrainTileImageLayerModelVector::iterator i = _model->colorLayers().begin();
                        i != _model->colorLayers().end();
                        ++i)
                    {
                        TerrainTileImageLayerModel* layerModel = i->get();
                        if ( layerModel && layerModel->getTexture() )
                        {
                            mptex->setLayer( layerModel->getImageLayer(), layerModel->getTexture() );
                        }
                    }

                    if ( !mptex->getPasses().empty() )
                    {
                        stateSet->setTextureAttribute(
                            colorBinding->unit(),
                            mptex );
                    }
                }
            }

            if ( _model->elevationModel().valid() && _model->elevationModel()->getTexture())
            {
                const SamplerBinding* binding = SamplerBinding::findUsage(bindings, SamplerBinding::ELEVATION);
                if ( binding )
                {                
                    stateSet->setTextureAttribute(
                        binding->unit(),
                        _model->elevationModel()->getTexture() );
                
                    stateSet->addUniform(
                        new osg::Uniform(binding->matrixName().c_str(),
                        osg::Matrixf::identity()));
                }
            }

            if ( _model->normalModel().valid() && _model->normalModel()->getTexture() )
            {
                const SamplerBinding* binding = SamplerBinding::findUsage(bindings, SamplerBinding::NORMAL);
                if ( binding )
                {
                    stateSet->setTextureAttribute(
                        binding->unit(),
                        _model->normalModel()->getTexture() );
                
                    stateSet->addUniform(
                        new osg::Uniform(binding->matrixName().c_str(),
                        osg::Matrixf::identity()));
                }
            }

            // Shared layers:
            for(TerrainTileImageLayerModelVector::iterator i = _model->sharedLayers().begin();
                i != _model->sharedLayers().end();
                ++i)
            {
                TerrainTileImageLayerModel* layerModel = i->get();
                if ( layerModel->getTexture() )
                {
                    const SamplerBinding* binding = SamplerBinding::findUID(bindings, layerModel->getImageLayer()->getUID());
                    if ( binding )
                    {
                        stateSet->setTextureAttribute(
                            binding->unit(),
                            layerModel->getTexture() );

                        stateSet->addUniform(
                            new osg::Uniform(binding->matrixName().c_str(),
                            osg::Matrixf::identity()));
                    }
                }
            }
        }
    }
}

void
LoadTileData::apply()
{
    if ( _model.valid() )
    {
        osg::ref_ptr<TileNode> tilenode;
        if ( _tilenode.lock(tilenode) )
        {
            const RenderBindings& bindings      = _context->getRenderBindings();
            const SelectionInfo&  selectionInfo = _context->getSelectionInfo();
            const MapInfo&  mapInfo             = _context->getMapFrame().getMapInfo();

            const SamplerBinding* color = SamplerBinding::findUsage(bindings, SamplerBinding::COLOR);

            // Find the mptexture, and then remove it since it was only in the state set for ICO compilation.d
            osg::ref_ptr<MPTexture> mptex = dynamic_cast<MPTexture*>(
                getStateSet()->getTextureAttribute( color->unit(), osg::StateAttribute::TEXTURE) );

            if ( mptex.valid() )
            {
                getStateSet()->removeTextureAttribute( color->unit(), mptex.get() );
            }

            // Merge our prepped stateset into the live one.
            tilenode->mergeStateSet( getStateSet(), mptex.get(), bindings);

            // Update existing inheritance matrices as necessary.
            UpdateInheritance update( bindings, selectionInfo, mapInfo, *_context->_options.tileSize() );
            tilenode->accept( update );

            // Mark as complete. TODO: per-data requests will do something different.
            tilenode->setDirty( false );

            // Delete the model immediately
            _model = 0L;
        }
        else
        {
            OE_WARN << LC << "LoadTileData failed; TileNode disappeared\n";
        }
    }
}
