// Filename: bulletGhostNode.cxx
// Created by:  enn0x (19Nov10)
//
////////////////////////////////////////////////////////////////////
//
// PANDA 3D SOFTWARE
// Copyright (c) Carnegie Mellon University.  All rights reserved.
//
// All use of this software is subject to the terms of the revised BSD
// license.  You should have received a copy of this license along
// with this source code in a file named "LICENSE."
//
////////////////////////////////////////////////////////////////////

#include "bulletGhostNode.h"
#include "bulletShape.h"

TypeHandle BulletGhostNode::_type_handle;

////////////////////////////////////////////////////////////////////
//     Function: BulletGhostNode::Constructor
//       Access: Published
//  Description:
////////////////////////////////////////////////////////////////////
BulletGhostNode::
BulletGhostNode(const char *name) : BulletBodyNode(name) {

  // Synchronised transform
  _sync = TransformState::make_identity();
  _sync_disable = false;

  // Initial transform
  btTransform trans = btTransform::getIdentity();

  // Ghost object
  _ghost = new btPairCachingGhostObject();
  _ghost->setUserPointer(this);
  _ghost->setCollisionFlags(btCollisionObject::CF_NO_CONTACT_RESPONSE);
  _ghost->setWorldTransform(trans);
  _ghost->setInterpolationWorldTransform(trans);
  _ghost->setCollisionShape(_shape);
}

////////////////////////////////////////////////////////////////////
//     Function: BulletGhostNode::get_object
//       Access: Published
//  Description:
////////////////////////////////////////////////////////////////////
btCollisionObject *BulletGhostNode::
get_object() const {

  return _ghost;
}

////////////////////////////////////////////////////////////////////
//     Function: BulletGhostNode::transform_changed
//       Access: Protected
//  Description:
////////////////////////////////////////////////////////////////////
void BulletGhostNode::
transform_changed() {

  if (_sync_disable) return;

  NodePath np = NodePath::any_path((PandaNode *)this);
  CPT(TransformState) ts = np.get_net_transform();

  LMatrix4f m_sync = _sync->get_mat();
  LMatrix4f m_ts = ts->get_mat();

  if (!m_sync.almost_equal(m_ts)) {
    _sync = ts;

    btTransform trans = TransformState_to_btTrans(ts);
    _ghost->setWorldTransform(trans);
    _ghost->setInterpolationWorldTransform(trans);

    if (ts->has_scale()) {
      LVecBase3f scale = ts->get_scale();
      for (int i=0; i<get_num_shapes(); i++) {
        PT(BulletShape) shape = _shapes[i];
        shape->set_local_scale(scale);
      }
    }
  }
}

////////////////////////////////////////////////////////////////////
//     Function: BulletGhostNode::sync_p2b
//       Access: Public
//  Description:
////////////////////////////////////////////////////////////////////
void BulletGhostNode::
sync_p2b() {

  transform_changed();
}

////////////////////////////////////////////////////////////////////
//     Function: BulletGhostNode::sync_b2p
//       Access: Public
//  Description:
////////////////////////////////////////////////////////////////////
void BulletGhostNode::
sync_b2p() {

  NodePath np = NodePath::any_path((PandaNode *)this);
  LVecBase3f scale = np.get_net_transform()->get_scale();

  btTransform trans = _ghost->getWorldTransform();
  CPT(TransformState) ts = btTrans_to_TransformState(trans, scale);

  LMatrix4f m_sync = _sync->get_mat();
  LMatrix4f m_ts = ts->get_mat();

  if (!m_sync.almost_equal(m_ts)) {
    _sync = ts;
    _sync_disable = true;
    np.set_transform(NodePath(), ts);
    _sync_disable = false;
  }
}

