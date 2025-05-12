#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
class Camera {
public:
    Camera(int windowWidth, int windowHeight);

    ~Camera();

    void processMouseScroll(float yOffset);

    void processOrbit(float xOffset, float yOffset);

    void updateViewMatrix();

    void updateProjectionMatrix(float aspectRatio);

    glm::mat4& getViewMatrix() {
        return m_viewMatrix;
    }

    glm::mat4& getProjectionMatrix() {
        return m_projectionMatrix;
    }

    glm::vec3 getPosition() const;

private:
    float m_radius = 5.0f; // Distance from the camera to the target
    float m_theta = 0.0f; // Horizontal angle
    float m_phi = 0.0f; // Vertical angle
    float m_orbitalSpeed = 0.005f; // Speed of orbiting around the target
    float m_zoomSpeed = 0.5f; // Speed of zooming in/out

    glm::vec3 m_target = glm::vec3(0.0f, 0.0f, 0.0f);
    glm::vec3 m_up = glm::vec3(0.0f, 1.0f, 0.0f);
    float m_fovYRadians = glm::radians(45.0f);
    float m_nearZ = 0.1f;
    float m_farZ = 100.0f;
    float m_aspectRatio = 16.0f / 9.0f; // Initial aspect ratio

    // Calculated matrices
    glm::mat4 m_viewMatrix;
    glm::mat4 m_projectionMatrix;
};
