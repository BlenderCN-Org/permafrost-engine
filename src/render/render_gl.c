#include "render_gl.h"
#include "render_private.h"
#include "mesh.h"
#include "vertex.h"
#include "shader.h"
#include "../entity.h"
#include "../gl_uniforms.h"

#include <GL/glew.h>

#include <stddef.h>

void GL_Init(struct render_private *priv)
{
    struct mesh *mesh = &priv->mesh;

    glGenVertexArrays(1, &mesh->VAO);
    glBindVertexArray(mesh->VAO);

    glGenBuffers(1, &mesh->VBO);
    glBindBuffer(GL_ARRAY_BUFFER, mesh->VBO);
    glBufferData(GL_ARRAY_BUFFER, mesh->num_verts * sizeof(struct vertex), mesh->vbuff, GL_STATIC_DRAW);

    /* Attribute 0 - position */
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(struct vertex), (void*)0);
    glEnableVertexAttribArray(0);  

    /* Attribute 1 - texture coordinates */
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(struct vertex), 
        (void*)offsetof(struct vertex, uv));
    glEnableVertexAttribArray(1);  

    //TODO: figure out attribute size constraints here
    /* Attribute 2 - joint weights */
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, sizeof(struct vertex), 
        (void*)offsetof(struct vertex, weights));
    glEnableVertexAttribArray(2);  

    priv->shader_prog = Shader_GetProgForName("generic");
}

void R_GL_Draw(struct entity *ent)
{
    struct render_private *priv = ent->render_private;
	GLuint loc;

    loc = glGetUniformLocation(priv->shader_prog, GL_U_MODEL);
	glUniformMatrix4fv(loc, 1, GL_FALSE, ent->model_matrix.raw);

    glUseProgram(priv->shader_prog);

    glBindVertexArray(priv->mesh.VAO);
    glDrawArrays(GL_TRIANGLES, 0, priv->mesh.num_verts);
}

void R_GL_SetView(const mat4x4_t *view, const char *shader_name)
{
    GLuint loc, shader_prog;

    shader_prog = Shader_GetProgForName(shader_name);

    loc = glGetUniformLocation(shader_prog, GL_U_VIEW);
	glUniformMatrix4fv(loc, 1, GL_FALSE, view->raw);
}

void R_GL_SetProj(const mat4x4_t *proj, const char *shader_name)
{
    GLuint loc, shader_prog;

    shader_prog = Shader_GetProgForName(shader_name);

    loc = glGetUniformLocation(shader_prog, GL_U_PROJECTION);
	glUniformMatrix4fv(loc, 1, GL_FALSE, proj->raw);
}
