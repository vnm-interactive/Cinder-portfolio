/*
 Copyright (C) 2011-2012 Gabor Papp

 This program is free software; you can redistribute it and/or modify
 it under the terms of the GNU Lesser General Public License as published
 by the Free Software Foundation; either version 3 of the License, or
 (at your option) any later version.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU Lesser General Public License for more details.

 You should have received a copy of the GNU Lesser General Public License
 along with this program. If not, see <http://www.gnu.org/licenses/>.

 Based on ofxAssimpModelLoader by Anton Marini, Memo Akten, Kyle McDonald
 and Arturo Castro
*/

#include <assert.h>

#include "../assimp--3.0.1270/include/assimp/Importer.hpp"
#include "../assimp--3.0.1270/include/assimp/scene.h"
#include "../assimp--3.0.1270/include/assimp/postprocess.h"

// for cinder builds, please add the corresponding path to linker configuration
// $(CINDER_PATH)\blocks\assimp\assimp--3.0.1270\lib\assimp_debug-dll_Win32\
// $(CINDER_PATH)\blocks\assimp\assimp--3.0.1270\lib\assimp_release-dll_Win32\

#pragma comment(lib, "assimp.lib")

#include "cinder/app/App.h"
#include "cinder/ImageIo.h"
#include "cinder/CinderMath.h"
#include "cinder/Utilities.h"
#include "cinder/TriMesh.h"
#include "cinder/gl/Texture.h"
#include "cinder/gl/Material.h"

#include <boost/make_shared.hpp>

#include "Scene.h"

using namespace std;
using namespace ci;

namespace cinder { namespace assimp {

class Mesh
{
public:
    const aiMesh *mAiMesh;

    gl::Texture mTexture;

    std::vector< uint32_t > mIndices;

    gl::Material mMaterial;
    bool mTwoSided;

    std::vector< aiVector3D > mAnimatedPos;
    std::vector< aiVector3D > mAnimatedNorm;

    std::string mName;
    TriMesh mCachedTriMesh;
    bool mValidCache;
};

static void fromAssimp( const aiMesh *aim, TriMesh *cim )
{
	// copy vertices
	for ( unsigned i = 0; i < aim->mNumVertices; ++i )
	{
		cim->appendVertex( fromAssimp( aim->mVertices[i] ) );
	}

	if( aim->HasNormals() )
	{
		for ( unsigned i = 0; i < aim->mNumVertices; ++i )
		{
			cim->appendNormal( fromAssimp( aim->mNormals[i] ) );
		}
	}

	// aiVector3D *	mTextureCoords [AI_MAX_NUMBER_OF_TEXTURECOORDS]
	// just one for now
	if ( aim->GetNumUVChannels() > 0 )
	{
		for ( unsigned i = 0; i < aim->mNumVertices; ++i )
		{
			cim->appendTexCoord( Vec2f( aim->mTextureCoords[0][i].x,
										aim->mTextureCoords[0][i].y ) );
		}
	}

	//aiColor4D *mColors [AI_MAX_NUMBER_OF_COLOR_SETS]
	if ( aim->GetNumColorChannels() > 0 )
	{
		for ( unsigned i = 0; i < aim->mNumVertices; ++i )
		{
			cim->appendColorRgba( fromAssimp( aim->mColors[0][i] ) );
		}
	}

	for ( unsigned i = 0; i < aim->mNumFaces; ++i )
	{
		if ( aim->mFaces[i].mNumIndices > 3 )
		{
			throw AssimpExc( "non-triangular face found: model " +
					string( aim->mName.data ) + ", face #" +
					toString< unsigned >( i ) );
		}

		cim->appendTriangle( aim->mFaces[ i ].mIndices[ 0 ],
							 aim->mFaces[ i ].mIndices[ 1 ],
							 aim->mFaces[ i ].mIndices[ 2 ] );
	}
}

Scene::Scene( fs::path filename ) :
	mMaterialsEnabled( false ),
	mTexturesEnabled( true ),
	mSkinningEnabled( false ),
	mAnimationEnabled( false ),
	mFilePath( filename ),
	mAnimationIndex( 0 )
{
	// FIXME: aiProcessPreset_TargetRealtime_MaxQuality contains
	// aiProcess_Debone which is buggy in 3.0.1270
	unsigned flags = aiProcess_Triangulate |
					 aiProcess_FlipUVs |
					 aiProcessPreset_TargetRealtime_Quality |
					 aiProcess_FindInstances |
					 aiProcess_ValidateDataStructure |
					 aiProcess_OptimizeMeshes;

    mImporterRef = boost::make_shared< Assimp::Importer >();
	mImporterRef->SetPropertyInteger( AI_CONFIG_PP_SBP_REMOVE,
			aiPrimitiveType_LINE | aiPrimitiveType_POINT );
	mImporterRef->SetPropertyInteger( AI_CONFIG_PP_PTV_NORMALIZE, true );

	mScene = mImporterRef->ReadFile( filename.string(), flags );
	if ( !mScene )
		throw AssimpExc( mImporterRef->GetErrorString() );

	calculateDimensions();

	loadAllMeshes();
	mRootNode = loadNodes( mScene->mRootNode );
}

void Scene::calculateDimensions()
{
	Vec3f aMin, aMax;
	calculateBoundingBox( &aMin, &aMax );
	mBoundingBox = AxisAlignedBox3f( aMin, aMax );
}

void Scene::calculateBoundingBox( Vec3f *min, Vec3f *max )
{
	aiMatrix4x4 trafo;

	aiVector3D aiMin, aiMax;
	aiMin.x = aiMin.y = aiMin.z =  1e10f;
	aiMax.x = aiMax.y = aiMax.z = -1e10f;

	calculateBoundingBoxForNode( mScene->mRootNode, &aiMin, &aiMax, &trafo );
	*min = fromAssimp( aiMin );
	*max = fromAssimp( aiMax );
}

void Scene::calculateBoundingBoxForNode( const aiNode *nd, aiVector3D *min, aiVector3D *max, aiMatrix4x4 *trafo )
{
	aiMatrix4x4 prev;

	prev = *trafo;
	*trafo = *trafo * nd->mTransformation;

	for ( unsigned n = 0; n < nd->mNumMeshes; ++n )
	{
		const struct aiMesh *mesh = mScene->mMeshes[ nd->mMeshes[ n ] ];
		for ( unsigned t = 0; t < mesh->mNumVertices; ++t )
		{
			aiVector3D tmp = mesh->mVertices[ t ];
			tmp *= (*trafo);

			min->x = math<float>::min( min->x, tmp.x );
			min->y = math<float>::min( min->y, tmp.y );
			min->z = math<float>::min( min->z, tmp.z );
			max->x = math<float>::max( max->x, tmp.x );
			max->y = math<float>::max( max->y, tmp.y );
			max->z = math<float>::max( max->z, tmp.z );
		}
	}

	for ( unsigned n = 0; n < nd->mNumChildren; ++n )
	{
		calculateBoundingBoxForNode( nd->mChildren[n], min, max, trafo );
	}

	*trafo = prev;
}

MeshNodeRef Scene::loadNodes( const aiNode *nd, MeshNodeRef parentRef )
{
	MeshNodeRef nodeRef = MeshNodeRef( new MeshNode() );
	nodeRef->setParent( parentRef );
	string nodeName = fromAssimp( nd->mName );
	nodeRef->setName( nodeName );
	mNodeMap[ nodeName] = nodeRef;
	mNodeNames.push_back( nodeName );

	// store transform
	aiVector3D scaling;
	aiQuaternion rotation;
	aiVector3D position;
	nd->mTransformation.Decompose( scaling, rotation, position );
	nodeRef->setScale( fromAssimp( scaling ) );
	nodeRef->setRotation( fromAssimp( rotation ) );
	nodeRef->setPosition( fromAssimp( position ) );

	// meshes
	for ( unsigned i = 0; i < nd->mNumMeshes; ++i )
	{
		unsigned meshId = nd->mMeshes[ i ];
		if ( meshId >= mMeshes.size() )
			throw AssimpExc( "node " + nodeRef->getName() + " references mesh #" +
					toString< unsigned >( meshId ) + " from " +
					toString< size_t >( mMeshes.size() ) + " meshes." );
		nodeRef->mMeshes.push_back( mMeshes[ meshId ] );
	}

	// store the node with meshes for rendering
	if ( nd->mNumMeshes > 0 )
	{
		mNodes.push_back( nodeRef );
	}

	// process all children
	for ( unsigned n = 0; n < nd->mNumChildren; ++n )
	{
		MeshNodeRef childRef = loadNodes( nd->mChildren[ n ], nodeRef );
		nodeRef->addChild( childRef );
	}
	return nodeRef;
}

MeshRef Scene::convertAiMesh( const aiMesh *mesh )
{
	// the current AssimpMesh we will be populating data into.
	MeshRef assimpMeshRef( new Mesh() );

	assimpMeshRef->mName = fromAssimp( mesh->mName );

	// Handle material info
	aiMaterial *mtl = mScene->mMaterials[ mesh->mMaterialIndex ];

	aiString name;
	mtl->Get( AI_MATKEY_NAME, name );
	app::console() << "material " << fromAssimp( name ) << endl;

	// Culling
	int twoSided;
	if ( ( AI_SUCCESS == mtl->Get( AI_MATKEY_TWOSIDED, twoSided ) ) && twoSided )
	{
		assimpMeshRef->mTwoSided = true;
		assimpMeshRef->mMaterial.setFace( GL_FRONT_AND_BACK );
		app::console() << " two sided" << endl;
	}
	else
	{
		assimpMeshRef->mTwoSided = false;
		assimpMeshRef->mMaterial.setFace( GL_FRONT );
	}

	aiColor4D dcolor, scolor, acolor, ecolor;
	if ( AI_SUCCESS == mtl->Get( AI_MATKEY_COLOR_DIFFUSE, dcolor ) )
	{
		assimpMeshRef->mMaterial.setDiffuse( fromAssimp( dcolor ) );
		app::console() << " diffuse: " << fromAssimp( dcolor ) << endl;
	}

	if ( AI_SUCCESS == mtl->Get( AI_MATKEY_COLOR_SPECULAR, scolor ) )
	{
		assimpMeshRef->mMaterial.setSpecular( fromAssimp( scolor ) );
		app::console() << " specular: " << fromAssimp( scolor ) << endl;
	}

	if ( AI_SUCCESS == mtl->Get( AI_MATKEY_COLOR_AMBIENT, acolor ) )
	{
		assimpMeshRef->mMaterial.setAmbient( fromAssimp( acolor ) );
		app::console() << " ambient: " << fromAssimp( acolor ) << endl;
	}

	if ( AI_SUCCESS == mtl->Get( AI_MATKEY_COLOR_EMISSIVE, ecolor ) )
	{
		assimpMeshRef->mMaterial.setEmission( fromAssimp( ecolor ) );
		app::console() << " emission: " << fromAssimp( ecolor ) << endl;
	}

	/*
	// FIXME: not sensible data, obj .mtl Ns 96.078431 -> 384.314
	float shininessStrength = 1;
	if ( AI_SUCCESS == mtl->Get( AI_MATKEY_SHININESS_STRENGTH, shininessStrength ) )
	{
		app::console() << "shininess strength: " << shininessStrength << endl;
	}
	float shininess;
	if ( AI_SUCCESS == mtl->Get( AI_MATKEY_SHININESS, shininess ) )
	{
		assimpMeshRef->mMaterial.setShininess( shininess * shininessStrength );
		app::console() << "shininess: " << shininess * shininessStrength << "[" <<
			shininess << "]" << endl;
	}
	*/

	// TODO: handle blending
#if 0
	int blendMode;
	if(AI_SUCCESS == aiGetMaterialInteger(mtl, AI_MATKEY_BLEND_FUNC, &blendMode)){
		if(blendMode==aiBlendMode_Default){
			meshHelper.blendMode=OF_BLENDMODE_ALPHA;
		}else{
			meshHelper.blendMode=OF_BLENDMODE_ADD;
		}
	}
#endif

	// Load Textures
	int texIndex = 0;
	aiString texPath;

	// TODO: handle other aiTextureTypes
	if ( AI_SUCCESS == mtl->GetTexture( aiTextureType_DIFFUSE, texIndex, &texPath ) )
	{
		app::console() << " diffuse texture " << texPath.data;
		fs::path texFsPath( texPath.data );
		fs::path modelFolder = mFilePath.parent_path();
		fs::path relTexPath = texFsPath.parent_path();
		fs::path texFile = texFsPath.filename();
		fs::path realPath = modelFolder / relTexPath / texFile;
		app::console() << " [" << realPath.string() << "]" << endl;

		// texture wrap
		gl::Texture::Format format;
		int uwrap;
		if ( AI_SUCCESS == mtl->Get( AI_MATKEY_MAPPINGMODE_U_DIFFUSE( 0 ), uwrap ) )
		{
			switch ( uwrap )
			{
				case aiTextureMapMode_Wrap:
					format.setWrapS( GL_REPEAT );
					break;

				case aiTextureMapMode_Clamp:
					format.setWrapS( GL_CLAMP );
					break;

				case aiTextureMapMode_Decal:
					// If the texture coordinates for a pixel are outside [0...1]
					// the texture is not applied to that pixel.
					format.setWrapS( GL_CLAMP_TO_EDGE );
					break;

				case aiTextureMapMode_Mirror:
					// A texture coordinate u|v becomes u%1|v%1 if (u-(u%1))%2
					// is zero and 1-(u%1)|1-(v%1) otherwise.
					// TODO
					format.setWrapS( GL_REPEAT );
					break;
			}
		}
		int vwrap;
		if ( AI_SUCCESS == mtl->Get( AI_MATKEY_MAPPINGMODE_V_DIFFUSE( 0 ), vwrap ) )
		{
			switch ( vwrap )
			{
				case aiTextureMapMode_Wrap:
					format.setWrapT( GL_REPEAT );
					break;

				case aiTextureMapMode_Clamp:
					format.setWrapT( GL_CLAMP );
					break;

				case aiTextureMapMode_Decal:
					// If the texture coordinates for a pixel are outside [0...1]
					// the texture is not applied to that pixel.
					format.setWrapT( GL_CLAMP_TO_EDGE );
					break;

				case aiTextureMapMode_Mirror:
					// A texture coordinate u|v becomes u%1|v%1 if (u-(u%1))%2
					// is zero and 1-(u%1)|1-(v%1) otherwise.
					// TODO
					format.setWrapT( GL_REPEAT );
					break;
			}
		}

        try 
        {
    		assimpMeshRef->mTexture = gl::Texture( loadImage( realPath ), format );
        }
        catch (ImageIoExceptionFailedLoad&)
        {
            app::console() << "Failed to load image from " << realPath << endl; 
        }
	}

	assimpMeshRef->mAiMesh = mesh;
	fromAssimp( mesh, &assimpMeshRef->mCachedTriMesh );
	assimpMeshRef->mValidCache = true;
	assimpMeshRef->mAnimatedPos.resize( mesh->mNumVertices );
	if ( mesh->HasNormals() )
	{
		assimpMeshRef->mAnimatedNorm.resize( mesh->mNumVertices );
	}


	assimpMeshRef->mIndices.resize( mesh->mNumFaces * 3 );
	unsigned j = 0;
	for ( unsigned x = 0; x < mesh->mNumFaces; ++x )
	{
		for ( unsigned a = 0; a < mesh->mFaces[x].mNumIndices; ++a)
		{
			assimpMeshRef->mIndices[ j++ ] = mesh->mFaces[ x ].mIndices[ a ];
		}
	}

	return assimpMeshRef;
}

void Scene::loadAllMeshes()
{
	app::console() << "loading model " << mFilePath.filename().string() <<
		" [" << mFilePath.string() << "] " << endl;
	for ( unsigned i = 0; i < mScene->mNumMeshes; ++i )
	{
		string name = fromAssimp( mScene->mMeshes[ i ]->mName );
		app::console() << "loading mesh " << i;
		if ( name != "" )
			app::console() << " [" << name << "]";
		app::console() << endl;
		MeshRef assimpMeshRef = convertAiMesh( mScene->mMeshes[ i ] );
		mMeshes.push_back( assimpMeshRef );
	}

#if 0
	animationTime = -1;
	setNormalizedTime(0);
#endif

	app::console() << "finished loading model " << mFilePath.filename().string() << endl;
}

void Scene::updateAnimation( size_t animationIndex, double currentTime )
{
	if ( mScene->mNumAnimations == 0 )
		return;

	const aiAnimation *mAnim = mScene->mAnimations[ animationIndex ];
	double ticks = mAnim->mTicksPerSecond;
	if ( ticks == 0.0 )
		ticks = 1.0;
	currentTime *= ticks;

	// calculate the transformations for each animation channel
	for( unsigned int a = 0; a < mAnim->mNumChannels; a++ )
	{
		const aiNodeAnim *channel = mAnim->mChannels[ a ];

		MeshNodeRef targetNode = getAssimpNode( fromAssimp( channel->mNodeName ) );

		// ******** Position *****
		aiVector3D presentPosition( 0, 0, 0 );
		if( channel->mNumPositionKeys > 0 )
		{
			// Look for present frame number. Search from last position if time is after the last time, else from beginning
			// Should be much quicker than always looking from start for the average use case.
			unsigned int frame = 0;// (currentTime >= lastAnimationTime) ? lastFramePositionIndex : 0;
			while( frame < channel->mNumPositionKeys - 1)
			{
				if( currentTime < channel->mPositionKeys[frame+1].mTime)
					break;
				frame++;
			}

			// interpolate between this frame's value and next frame's value
			unsigned int nextFrame = (frame + 1) % channel->mNumPositionKeys;
			const aiVectorKey& key = channel->mPositionKeys[frame];
			const aiVectorKey& nextKey = channel->mPositionKeys[nextFrame];
			double diffTime = nextKey.mTime - key.mTime;
			if( diffTime < 0.0)
				diffTime += mAnim->mDuration;
			if( diffTime > 0)
			{
				float factor = float( (currentTime - key.mTime) / diffTime);
				presentPosition = key.mValue + (nextKey.mValue - key.mValue) * factor;
			} else
			{
				presentPosition = key.mValue;
			}
		}

		// ******** Rotation *********
		aiQuaternion presentRotation( 1, 0, 0, 0);
		if( channel->mNumRotationKeys > 0)
		{
			unsigned int frame = 0;//(currentTime >= lastAnimationTime) ? lastFrameRotationIndex : 0;
			while( frame < channel->mNumRotationKeys - 1)
			{
				if( currentTime < channel->mRotationKeys[frame+1].mTime)
					break;
				frame++;
			}

			// interpolate between this frame's value and next frame's value
			unsigned int nextFrame = (frame + 1) % channel->mNumRotationKeys;
			const aiQuatKey& key = channel->mRotationKeys[frame];
			const aiQuatKey& nextKey = channel->mRotationKeys[nextFrame];
			double diffTime = nextKey.mTime - key.mTime;
			if( diffTime < 0.0)
				diffTime += mAnim->mDuration;
			if( diffTime > 0)
			{
				float factor = float( (currentTime - key.mTime) / diffTime);
				aiQuaternion::Interpolate( presentRotation, key.mValue, nextKey.mValue, factor);
			} else
			{
				presentRotation = key.mValue;
			}
		}

		// ******** Scaling **********
		aiVector3D presentScaling( 1, 1, 1);
		if( channel->mNumScalingKeys > 0)
		{
			unsigned int frame = 0;//(currentTime >= lastAnimationTime) ? lastFrameScaleIndex : 0;
			while( frame < channel->mNumScalingKeys - 1)
			{
				if( currentTime < channel->mScalingKeys[frame+1].mTime)
					break;
				frame++;
			}

			// TODO: (thom) interpolation maybe? This time maybe even logarithmic, not linear
			presentScaling = channel->mScalingKeys[frame].mValue;
		}

		targetNode->setRotation( fromAssimp( presentRotation ) );
		targetNode->setScale( fromAssimp( presentScaling ) );
		targetNode->setPosition( fromAssimp( presentPosition ) );
	}
}

MeshNodeRef Scene::getAssimpNode( const std::string &name )
{
	map< string, MeshNodeRef >::iterator i = mNodeMap.find( name );
	if ( i != mNodeMap.end() )
		return i->second;
	else
		return MeshNodeRef();
}

const MeshNodeRef Scene::getAssimpNode( const std::string &name ) const
{
	map< string, MeshNodeRef >::const_iterator i = mNodeMap.find( name );
	if ( i != mNodeMap.end() )
		return i->second;
	else
		return MeshNodeRef();
}

size_t Scene::getAssimpNodeNumMeshes( const string &name )
{
	MeshNodeRef node = getAssimpNode( name );
	if ( node )
		return node->mMeshes.size();
	else
		return 0;
}

TriMesh& Scene::getAssimpNodeMesh( const string &name, size_t n /* = 0 */ )
{
	MeshNodeRef node = getAssimpNode( name );
	if ( node && n < node->mMeshes.size() )
		return node->mMeshes[ n ]->mCachedTriMesh;
	else
		throw AssimpExc( "node " + name + " not found." );
}

const TriMesh& Scene::getAssimpNodeMesh( const string &name, size_t n /* = 0 */ ) const
{
	const MeshNodeRef node = getAssimpNode( name );
	if ( node && n < node->mMeshes.size() )
		return node->mMeshes[ n ]->mCachedTriMesh;
	else
		throw AssimpExc( "node " + name + " not found." );
}

gl::Texture& Scene::getAssimpNodeTexture( const string &name, size_t n /* = 0 */ )
{
	MeshNodeRef node = getAssimpNode( name );
	if ( node && n < node->mMeshes.size() )
		return node->mMeshes[ n ]->mTexture;
	else
		throw AssimpExc( "node " + name + " not found." );
}

const gl::Texture& Scene::getAssimpNodeTexture( const string &name, size_t n /* = 0 */ ) const
{
	const MeshNodeRef node = getAssimpNode( name );
	if ( node && n < node->mMeshes.size() )
		return node->mMeshes[ n ]->mTexture;
	else
		throw AssimpExc( "node " + name + " not found." );
}

gl::Material& Scene::getAssimpNodeMaterial( const string &name, size_t n /* = 0 */ )
{
	MeshNodeRef node = getAssimpNode( name );
	if ( node && n < node->mMeshes.size() )
		return node->mMeshes[ n ]->mMaterial;
	else
		throw AssimpExc( "node " + name + " not found." );
}

const gl::Material& Scene::getAssimpNodeMaterial( const string &name, size_t n /* = 0 */ ) const
{
	const MeshNodeRef node = getAssimpNode( name );
	if ( node && n < node->mMeshes.size() )
		return node->mMeshes[ n ]->mMaterial;
	else
		throw AssimpExc( "node " + name + " not found." );
}

void Scene::setNodeOrientation( const string &name, const Quatf &rot )
{
	MeshNodeRef node = getAssimpNode( name );
	if ( node )
		node->setRotation( rot );
}

Quatf Scene::getNodeOrientation( const string &name )
{
	MeshNodeRef node = getAssimpNode( name );
	if ( node )
		return node->getRotation();
	else
		return Quatf();
}

size_t Scene::getNumAnimations() const
{
	return mScene->mNumAnimations;
}

void Scene::setAnimation( size_t n )
{
	mAnimationIndex = math< size_t >::clamp( n, 0, getNumAnimations() );
}

void Scene::setTime( double t )
{
	mAnimationTime = t;
}

double Scene::getAnimationDuration( size_t n ) const
{
	const aiAnimation *anim = mScene->mAnimations[ n ];
	double ticks = anim->mTicksPerSecond;
	if ( ticks == 0.0 )
		ticks = 1.0;
	return anim->mDuration / ticks;
}

void Scene::updateSkinning()
{
	vector< MeshNodeRef >::const_iterator it = mNodes.begin();
	for ( ; it != mNodes.end(); ++it )
	{
		MeshNodeRef nodeRef = *it;

		vector< MeshRef >::const_iterator meshIt = nodeRef->mMeshes.begin();
		for ( ; meshIt != nodeRef->mMeshes.end(); ++meshIt )
		{
			MeshRef assimpMeshRef = *meshIt;

			// current mesh we are introspecting
			const aiMesh *mesh = assimpMeshRef->mAiMesh;

			// calculate bone matrices
			std::vector< aiMatrix4x4 > boneMatrices( mesh->mNumBones );
			for ( unsigned a = 0; a < mesh->mNumBones; ++a )
			{
				const aiBone *bone = mesh->mBones[ a ];

				// find the corresponding node by again looking recursively through
				// the node hierarchy for the same name
				MeshNodeRef nodeRef = getAssimpNode( fromAssimp( bone->mName ) );
				assert( nodeRef );
				// start with the mesh-to-bone matrix
				// and append all node transformations down the parent chain until
				// we're back at mesh coordinates again
				boneMatrices[ a ] = toAssimp( nodeRef->getWorldTransform() ) *
										bone->mOffsetMatrix;
			}

			assimpMeshRef->mValidCache = false;

			assimpMeshRef->mAnimatedPos.assign( assimpMeshRef->mAnimatedPos.size(),
					aiVector3D( 0, 0, 0 ) );
			if ( mesh->HasNormals() )
			{
				assimpMeshRef->mAnimatedNorm.assign( assimpMeshRef->mAnimatedNorm.size(),
						aiVector3D( 0, 0, 0 ) );
			}

			// loop through all vertex weights of all bones
			for ( unsigned a = 0; a < mesh->mNumBones; ++a )
			{
				const aiBone *bone = mesh->mBones[a];
				const aiMatrix4x4 &posTrafo = boneMatrices[ a ];

				for ( unsigned b = 0; b < bone->mNumWeights; ++b )
				{
					const aiVertexWeight &weight = bone->mWeights[ b ];
					size_t vertexId = weight.mVertexId;
					const aiVector3D& srcPos = mesh->mVertices[vertexId];

					assimpMeshRef->mAnimatedPos[vertexId] += weight.mWeight * (posTrafo * srcPos);
				}

				if ( mesh->HasNormals() )
				{
					// 3x3 matrix, contains the bone matrix without the
					// translation, only with rotation and possibly scaling
					aiMatrix3x3 normTrafo = aiMatrix3x3( posTrafo );
					for ( size_t b = 0; b < bone->mNumWeights; ++b )
					{
						const aiVertexWeight &weight = bone->mWeights[b];
						size_t vertexId = weight.mVertexId;

						const aiVector3D& srcNorm = mesh->mNormals[vertexId];
						assimpMeshRef->mAnimatedNorm[vertexId] += weight.mWeight * (normTrafo * srcNorm);
					}
				}
			}
		}
	}
}

void Scene::updateMeshes()
{
	vector< MeshNodeRef >::iterator it = mNodes.begin();
	for ( ; it != mNodes.end(); ++it )
	{
		MeshNodeRef nodeRef = *it;

		vector< MeshRef >::iterator meshIt = nodeRef->mMeshes.begin();
		for ( ; meshIt != nodeRef->mMeshes.end(); ++meshIt )
		{
			MeshRef assimpMeshRef = *meshIt;

			if ( assimpMeshRef->mValidCache )
				continue;

			if ( mSkinningEnabled )
			{
				// animated data
				std::vector< Vec3f > &vertices = assimpMeshRef->mCachedTriMesh.getVertices();
				for( size_t v = 0; v < vertices.size(); ++v )
					vertices[v] = fromAssimp( assimpMeshRef->mAnimatedPos[ v ] );

				std::vector< Vec3f > &normals = assimpMeshRef->mCachedTriMesh.getNormals();
				for( size_t v = 0; v < normals.size(); ++v )
					normals[v] = fromAssimp( assimpMeshRef->mAnimatedNorm[ v ] );
			}
			else
			{
				// original mesh data from assimp
				const aiMesh *mesh = assimpMeshRef->mAiMesh;

				std::vector< Vec3f > &vertices = assimpMeshRef->mCachedTriMesh.getVertices();
				for( size_t v = 0; v < vertices.size(); ++v )
					vertices[v] = fromAssimp( mesh->mVertices[ v ] );

				std::vector< Vec3f > &normals = assimpMeshRef->mCachedTriMesh.getNormals();
				for( size_t v = 0; v < normals.size(); ++v )
					normals[v] = fromAssimp( mesh->mNormals[ v ] );
			}

			assimpMeshRef->mValidCache = true;
		}
	}
}

void Scene::enableSkinning( bool enable /* = true */ )
{
	if ( mSkinningEnabled == enable )
		return;

	mSkinningEnabled = enable;
	// invalidate mesh cache
	vector< MeshNodeRef >::const_iterator it = mNodes.begin();
	for ( ; it != mNodes.end(); ++it )
	{
		MeshNodeRef nodeRef = *it;

		vector< MeshRef >::const_iterator meshIt = nodeRef->mMeshes.begin();
		for ( ; meshIt != nodeRef->mMeshes.end(); ++meshIt )
		{
			MeshRef assimpMeshRef = *meshIt;
			assimpMeshRef->mValidCache = false;
		}
	}
}

void Scene::update()
{
	if ( mAnimationEnabled )
		updateAnimation( mAnimationIndex, mAnimationTime );

	if ( mSkinningEnabled )
		updateSkinning();

	updateMeshes();
}

void Scene::draw()
{
	glPushAttrib( GL_ALL_ATTRIB_BITS );
	glPushClientAttrib( GL_CLIENT_ALL_ATTRIB_BITS );
	gl::enable( GL_NORMALIZE );

	vector< MeshNodeRef >::const_iterator it = mNodes.begin();
	for ( ; it != mNodes.end(); ++it )
	{
		MeshNodeRef nodeRef = *it;

		vector< MeshRef >::const_iterator meshIt = nodeRef->mMeshes.begin();
		for ( ; meshIt != nodeRef->mMeshes.end(); ++meshIt )
		{
			MeshRef assimpMeshRef = *meshIt;

			// Texture Binding
			if ( mTexturesEnabled && assimpMeshRef->mTexture )
			{
				assimpMeshRef->mTexture.enableAndBind();
			}

			if ( mMaterialsEnabled )
			{
				assimpMeshRef->mMaterial.apply();
			}
			else
			{
				//gl::color( assimpMeshRef->mMaterial.getDiffuse());
			}

			// Culling
			if ( assimpMeshRef->mTwoSided )
				gl::enable( GL_CULL_FACE );
			else
				gl::disable( GL_CULL_FACE );

			gl::draw( assimpMeshRef->mCachedTriMesh );

			// Texture Binding
			if ( mTexturesEnabled && assimpMeshRef->mTexture )
			{
				assimpMeshRef->mTexture.unbind();
			}
		}
	}

	glPopClientAttrib();
	glPopAttrib();
}

TriMesh & Scene::getMesh( size_t n )
{
    return mMeshes[ n ]->mCachedTriMesh;
}

const TriMesh & Scene::getMesh( size_t n ) const
{
    return mMeshes[ n ]->mCachedTriMesh;
}

gl::Texture & Scene::getTexture( size_t n )
{
    return mMeshes[ n ]->mTexture;
}

const gl::Texture & Scene::getTexture( size_t n ) const
{
    return mMeshes[ n ]->mTexture;
}

} } // namespace cinder::assimp

