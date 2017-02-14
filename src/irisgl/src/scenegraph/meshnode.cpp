/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016  GPLv3 Jahshaka LLC <coders@jahshaka.com>

This is free software: you may copy, redistribute
and/or modify it under the terms of the GPLv3 License

For more information see the LICENSE file
*************************************************************************/

#include <QFileInfo>
#include <QDir>

#include "meshnode.h"
#include "../graphics/mesh.h"
#include "assimp/postprocess.h"
#include "assimp/Importer.hpp"
#include "assimp/scene.h"
#include "assimp/mesh.h"
#include "assimp/material.h"
#include "assimp/matrix4x4.h"
#include "assimp/vector3.h"
#include "assimp/quaternion.h"

#include "../graphics/vertexlayout.h"
#include "../materials/defaultmaterial.h"
#include "../materials/materialhelper.h"
#include "../graphics/renderitem.h"

#include "../core/scene.h"
#include "../core/scenenode.h"

namespace iris
{

int MeshNode::index = 0;

MeshNode::MeshNode() : m_index(index++) {
    mesh = nullptr;
    sceneNodeType = SceneNodeType::Mesh;

    texture = iris::Texture2D::load(
                IrisUtils::getAbsoluteAssetPath("assets/textures/default_particle.jpg")
    );
    pps = 24;
    speed = 12;
    gravity = .0f;
    particleLife = 5;

    scaleFac = 0.1f;
    lifeFac = 0.0f;
    speedFac = 0.0f;

    useAdditive = false;
    randomRotation = true;
    dissipate = false;

    renderItem = new RenderItem();
    renderItem->type = RenderItemType::Mesh;
}

// @todo: cleanup previous mesh item
void MeshNode::setMesh(QString source)
{
    meshPath = source;
    mesh = Mesh::loadMesh(source);
    meshPath = source;
    meshIndex = 0;

    renderItem->mesh = mesh;
}

//should not be used on plain scene meshes
void MeshNode::setMesh(Mesh* mesh)
{
    this->mesh = mesh;
}

Mesh* MeshNode::getMesh()
{
    return mesh;
}

void MeshNode::setMaterial(MaterialPtr material)
{
    this->material = material;

    renderItem->matrial = material;
}

void MeshNode::submitRenderItems()
{
    this->scene->geometryRenderList.append(renderItem);
    this->scene->shadowRenderList.append(renderItem);
}

/**
 * Recursively builds a SceneNode/MeshNode heirarchy from the aiScene of the loaded model
 * todo: read and apply material data
 * @param scene
 * @param node
 * @return
 */
QSharedPointer<iris::SceneNode> _buildScene(const aiScene* scene,aiNode* node,QString filePath)
{
    QSharedPointer<iris::SceneNode> sceneNode;// = QSharedPointer<iris::SceneNode>(new iris::SceneNode());

    // if this node only has one child then make sceneNode a meshnode and add mesh to it
    if(node->mNumMeshes==1)
    {
        auto meshNode = iris::MeshNode::create();
        auto mesh = scene->mMeshes[node->mMeshes[0]];

        // objects like Bezier curves have no vertex positions in the aiMesh
        // aside from that, iris currently only renders meshes
        if(mesh->HasPositions())
        {
            meshNode->setMesh(new Mesh(mesh));
            meshNode->name = QString(mesh->mName.C_Str());
            meshNode->meshPath = filePath;
            meshNode->meshIndex = node->mMeshes[0];

            // mesh->mMaterialIndex is always at least 0
            auto m = scene->mMaterials[mesh->mMaterialIndex];
            auto dir = QFileInfo(filePath).absoluteDir().absolutePath();

            auto meshMat = iris::MaterialHelper::createMaterial(m, dir);
            if(!!meshMat)
                meshNode->setMaterial(meshMat);
            else
                meshNode->setMaterial(iris::DefaultMaterial::create());

        }
        sceneNode = meshNode;
    }
    else
    {
        //otherwise, add meshes as child nodes
        sceneNode = QSharedPointer<iris::SceneNode>(new iris::SceneNode());

        for(unsigned i=0;i<node->mNumMeshes;i++)
        {
            auto mesh = scene->mMeshes[node->mMeshes[i]];

            auto meshNode = iris::MeshNode::create();
            meshNode->name = QString(mesh->mName.C_Str());
            meshNode->meshPath = filePath;
            meshNode->meshIndex = node->mMeshes[i];

            meshNode->setMesh(new Mesh(mesh));
            sceneNode->addChild(meshNode);

            //apply material
            //meshNode->setMaterial(iris::DefaultMaterial::create());
            auto m = scene->mMaterials[mesh->mMaterialIndex];
            auto dir = QFileInfo(filePath).absoluteDir().absolutePath();

            auto meshMat = iris::MaterialHelper::createMaterial(m, dir);
            if(!!meshMat)
                meshNode->setMaterial(meshMat);
            else
                meshNode->setMaterial(iris::DefaultMaterial::create());
        }

    }

    //extract transform
    aiVector3D pos,scale;
    aiQuaternion rot;

    //auto transform = node->mTransformation;
    node->mTransformation.Decompose(scale,rot,pos);
    sceneNode->pos = QVector3D(pos.x,pos.y,pos.z);
    sceneNode->scale = QVector3D(scale.x,scale.y,scale.z);

    rot.Normalize();
    sceneNode->rot = QQuaternion(rot.w,rot.x,rot.y,rot.z);//not sure if this is correct

    for(unsigned i=0;i<node->mNumChildren;i++)
    {
        auto child = _buildScene(scene,node->mChildren[i],filePath);
        sceneNode->addChild(child);
    }

    return sceneNode;
}

QSharedPointer<iris::SceneNode> MeshNode::loadAsSceneFragment(QString filePath)
{
    Assimp::Importer importer;
    const aiScene *scene = importer.ReadFile(filePath.toStdString().c_str(),aiProcessPreset_TargetRealtime_Fast);

    if(!scene)
        return QSharedPointer<iris::MeshNode>(nullptr);

    if(scene->mNumMeshes==0)
        return QSharedPointer<iris::MeshNode>(nullptr);

    if(scene->mNumMeshes==1)
    {
        auto mesh = scene->mMeshes[0];
        auto node = iris::MeshNode::create();
        node->setMesh(new Mesh(scene->mMeshes[0]));
        node->meshPath = filePath;
        node->meshIndex = 0;

        //mesh->mMaterialIndex is always >= 0
        auto m = scene->mMaterials[mesh->mMaterialIndex];
        auto dir = QFileInfo(filePath).absoluteDir().absolutePath();

        auto meshMat = MaterialHelper::createMaterial(m,dir);
        if(!!meshMat)
            node->setMaterial(meshMat);

        return node;
    }

    auto node = _buildScene(scene,scene->mRootNode,filePath);

    return node;
}

}
