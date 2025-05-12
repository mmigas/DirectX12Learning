#include "Camera.hpp"

#include <algorithm>

Camera::Camera(int windowWidth, int windowHeight) {
    m_aspectRatio = static_cast<float>(windowWidth) / static_cast<float>(windowHeight);
    updateProjectionMatrix(m_aspectRatio);
    updateViewMatrix();
}

Camera::~Camera() {
}

void Camera::processMouseScroll(float yOffset) {
    // Adjust radius based on scroll wheel delta
    m_radius -= yOffset * m_zoomSpeed;
    // Clamp radius to prevent going inside the target or too far
    m_radius = std::max(0.5f, m_radius); // Minimum radius
    m_radius = std::min(50.0f, m_radius); // Maximum radius
}

void Camera::processOrbit(float xOffset, float yOffset) {
    // Adjust angles based on mouse movement
    m_theta -= xOffset * m_orbitalSpeed;
    m_phi += yOffset * m_orbitalSpeed; // Subtract yoffset to make up drag feel natural

    // Clamp phi to prevent looking straight up or down (and flipping)
    // Allow slightly more than +/- 90 degrees to avoid gimbal lock issues near poles
    const float phiMax = glm::radians(89.0f);
    const float phiMin = glm::radians(-89.0f);
    m_phi = std::max(phiMin, std::min(phiMax, m_phi));

    // Keep theta within 0 to 2*PI (optional, avoids large numbers)
    // m_theta = fmod(m_theta, 2.0f * M_PI);

    // No need to update view matrix here, will be done in Update()
}

void Camera::updateViewMatrix() {
    glm::vec3 position = getPosition();
    m_viewMatrix = lookAt(position, m_target, m_up);
}

void Camera::updateProjectionMatrix(float aspectRatio) {
    m_aspectRatio = aspectRatio;
    m_projectionMatrix = glm::perspective(m_fovYRadians, m_aspectRatio, m_nearZ, m_farZ);
}

glm::vec3 Camera::getPosition() const {
    float x = m_radius * cos(m_phi) * sin(m_theta);
    float y = m_radius * sin(m_phi);
    float z = m_radius * cos(m_phi) * cos(m_theta);
    return m_target + glm::vec3(x, y, z);
}
